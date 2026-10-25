#include "console.h"
#include <bluefruit.h>

ConsoleOut Console;

static BLEUart s_uart;
static volatile uint16_t s_phone_conn = BLE_CONN_HANDLE_INVALID;
static SemaphoreHandle_t s_lock;

static char    s_tx_line[128];
static uint8_t s_tx_len;

// PIN pairing gives an authenticated (MITM-protected) link: security level 3+
static bool phone_authenticated(void)
{
    uint16_t conn = s_phone_conn;
    if (conn == BLE_CONN_HANDLE_INVALID) return false;
    ble_gap_conn_sec_t sec;
    if (sd_ble_gap_conn_sec_get(conn, &sec) != NRF_SUCCESS) return false;
    return sec.sec_mode.sm == 1 && sec.sec_mode.lv >= 3;
}

static void ble_flush_line(void)
{
    if (s_tx_len && phone_authenticated() && s_uart.notifyEnabled(s_phone_conn)) {
        s_uart.write(s_phone_conn, (const uint8_t *)s_tx_line, s_tx_len);
    }
    s_tx_len = 0;
}

size_t ConsoleOut::write(uint8_t c)
{
    return write(&c, 1);
}

size_t ConsoleOut::write(const uint8_t *buf, size_t len)
{
    Serial.write(buf, len);
    if (!s_lock) return len;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        s_tx_line[s_tx_len++] = buf[i];
        if (buf[i] == '\n' || s_tx_len == sizeof(s_tx_line)) ble_flush_line();
    }
    xSemaphoreGive(s_lock);
    return len;
}

// Phone terminal app subscribed to UART: remember it and ask it to pair (phone shows a PIN prompt)
static void on_uart_notify(uint16_t conn_handle, bool enabled)
{
    if (!enabled) return;
    s_phone_conn = conn_handle;
    BLEConnection *conn = Bluefruit.Connection(conn_handle);
    if (conn && !phone_authenticated()) conn->requestPairing();
}

static void on_secured(uint16_t conn_handle)
{
    if (conn_handle == s_phone_conn && phone_authenticated()) {
        Console.println("Phone paired. Type 'help' for commands.");
    }
}

static void on_disconnect(uint16_t conn_handle)
{
    if (conn_handle == s_phone_conn) s_phone_conn = BLE_CONN_HANDLE_INVALID;
}

void console_init(const char *pin)
{
    s_lock = xSemaphoreCreateMutex();
    console_set_pin(pin);
    Bluefruit.Security.setSecuredCallback(on_secured);
    s_uart.setNotifyCallback(on_uart_notify);
    s_uart.begin();
}

void console_set_pin(const char *pin)
{
    Bluefruit.Security.setPIN(pin);
}

bool console_is_phone(uint16_t conn_handle)
{
    return conn_handle == s_phone_conn;
}

void console_add_adv_uuid(void)
{
    Bluefruit.ScanResponse.addService(s_uart);
}

static void line_input(char c, char *line, uint8_t *len, size_t cap, console_cmd_cb_t handler)
{
    if (c == '\r' || c == '\n') {
        if (*len) {
            line[*len] = '\0';
            *len = 0;
            handler(line);
        }
    } else if (*len < cap - 1) {
        line[(*len)++] = c;
    }
}

void console_poll(console_cmd_cb_t handler)
{
    static char usb_line[64], ble_line[64];
    static uint8_t usb_len, ble_len;

    while (Serial.available()) {
        line_input(Serial.read(), usb_line, &usb_len, sizeof(usb_line), handler);
    }

    // Drop the phone handle if that link went away
    uint16_t phone = s_phone_conn;
    if (phone != BLE_CONN_HANDLE_INVALID && !Bluefruit.connected(phone)) on_disconnect(phone);

    bool authed = phone_authenticated();
    bool warned = false;
    while (s_uart.available()) {
        char c = s_uart.read();
        if (!authed) {
            // Reject everything until PIN pairing is done; nudge the phone to pair
            if (!warned && s_phone_conn != BLE_CONN_HANDLE_INVALID) {
                Serial.println("[ble] Command from unpaired phone ignored");
                BLEConnection *conn = Bluefruit.Connection(s_phone_conn);
                if (conn) conn->requestPairing();
                warned = true;
            }
            continue;
        }
        line_input(c, ble_line, &ble_len, sizeof(ble_line), handler);
    }
}
