#include "ftms_client.h"
#include <bluefruit.h>
#include "console.h"
#include "ftms_parse.h"
#include <string.h>

// FTMS Control Point op codes
#define FTMS_CP_REQUEST_CONTROL    0x00
#define FTMS_CP_SET_TARGET_SPEED   0x02
#define FTMS_CP_SET_TARGET_INCLINE 0x03
#define FTMS_CP_START_RESUME       0x07
#define FTMS_CP_STOP_PAUSE         0x08
#define FTMS_CP_RESPONSE           0x80

#define SCAN_USER_MS         10000
#define SCAN_AUTO_MS         5000
#define AUTO_RETRY_MS        3000
#define CONNECT_TIMEOUT_MS   6000
#define RECONNECT_MAX_MS     60000
#define RECONNECT_FAST_MS    2000

static BLEClientService        s_ftms_svc(0x1826);
static BLEClientCharacteristic s_tm_data(0x2ACD);
static BLEClientCharacteristic s_ctrl_point(0x2AD9);

static ftms_data_cb_t s_data_cb;
static ftms_conn_cb_t s_conn_cb;

static volatile ftms_state_t s_state = FTMS_STATE_IDLE;
static volatile uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;
static volatile bool s_control_acquired;

// Merged treadmill state: some treadmills (e.g. Woodway) split fields across notifications
static ftms_treadmill_data_t s_latest;
static bool s_raw_log;

// Target treadmill (for connect/reconnect)
static uint8_t  s_target_addr[6];
static uint8_t  s_target_addr_type;
static bool     s_have_target;
static bool     s_should_reconnect;
static uint32_t s_reconnect_delay_ms = RECONNECT_FAST_MS;
static uint32_t s_next_scan_ms;
static uint32_t s_state_since_ms;
static uint16_t s_reconnect_attempts;
static bool     s_had_working_connection;

// User scan results
static ftms_scan_result_t s_scan_results[FTMS_MAX_SCAN_RESULTS];
static volatile uint8_t   s_scan_count;
static bool     s_user_scan;
static bool     s_auto_pick;          // pick the nearest treadmill when the scan ends
static uint32_t s_auto_retry_ms;
static uint32_t s_user_scan_end_ms;

static void set_state(ftms_state_t st)
{
    if (s_state != st) {
        s_state = st;
        s_state_since_ms = millis();
    }
}

static void on_tm_data(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len)
{
    (void)chr;
    if (s_raw_log) {
        Console.print("[raw]");
        for (uint16_t i = 0; i < len; i++) Console.printf(" %02X", data[i]);
        Console.println();
    }
    if (ftms_parse_treadmill_data(data, len, &s_latest) && s_data_cb) s_data_cb(&s_latest);
}

static void on_ctrl_indicate(BLEClientCharacteristic *chr, uint8_t *data, uint16_t len)
{
    (void)chr;
    if (len < 3 || data[0] != FTMS_CP_RESPONSE) return;

    bool ok = (data[2] == 0x01);
    if (data[1] == FTMS_CP_REQUEST_CONTROL) {
        s_control_acquired = ok;
        Console.printf("[ftms] Control %s\n", ok ? "acquired" : "denied");
    } else if (!ok) {
        Console.printf("[ftms] Control point op 0x%02X failed (result 0x%02X)\n", data[1], data[2]);
    }
}

static void begin_connect(const ble_gap_evt_adv_report_t *report)
{
    Bluefruit.Scanner.stop();
    // Connect uses the scanner's timeout as its own; bound it so we never wait forever
    Bluefruit.Scanner.getParams()->timeout = CONNECT_TIMEOUT_MS / 10;
    set_state(FTMS_STATE_CONNECTING);
    if (!Bluefruit.Central.connect(report)) {
        Console.println("[ftms] Connect request failed");
        set_state(s_had_working_connection || s_should_reconnect ? FTMS_STATE_RECONNECTING : FTMS_STATE_IDLE);
        s_next_scan_ms = millis() + s_reconnect_delay_ms;
    }
}

static void on_scan_report(ble_gap_evt_adv_report_t *report)
{
    const uint8_t *addr = report->peer_addr.addr;
    bool is_ftms = Bluefruit.Scanner.checkReportForUuid(report, BLEUuid(0x1826));

    char name[sizeof(s_scan_results[0].name)] = {0};
    if (!Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME,
                                             (uint8_t *)name, sizeof(name) - 1)) {
        Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,
                                            (uint8_t *)name, sizeof(name) - 1);
    }

    // Reconnect / connect-to-saved mode: pounce on the target as soon as we see it
    if (!s_user_scan && s_should_reconnect && s_have_target) {
        if (memcmp(addr, s_target_addr, 6) == 0 && is_ftms) {
            begin_connect(report);
            return;
        }
        Bluefruit.Scanner.resume();
        return;
    }

    // User scan: collect FTMS devices; scan responses (no UUID) can fill in names
    bool found = false;
    for (uint8_t i = 0; i < s_scan_count; i++) {
        ftms_scan_result_t *r = &s_scan_results[i];
        if (memcmp(r->addr, addr, 6) != 0) continue;
        found = true;
        if (name[0] && !r->name[0]) strncpy(r->name, name, sizeof(r->name) - 1);
        if (report->rssi > r->rssi) r->rssi = report->rssi;
        break;
    }
    if (!found && is_ftms && s_scan_count < FTMS_MAX_SCAN_RESULTS) {
        ftms_scan_result_t *r = &s_scan_results[s_scan_count];
        memset(r, 0, sizeof(*r));
        memcpy(r->addr, addr, 6);
        r->addr_type = report->peer_addr.addr_type;
        r->rssi = report->rssi;
        strncpy(r->name, name, sizeof(r->name) - 1);
        s_scan_count = s_scan_count + 1;
    }
    Bluefruit.Scanner.resume();
}

static void on_central_connect(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    s_latest = ftms_treadmill_data_t{};
    s_control_acquired = false;
    set_state(FTMS_STATE_DISCOVERING);
    Console.println("[ftms] Connected, discovering FTMS...");

    BLEConnection *conn = Bluefruit.Connection(conn_handle);

    if (!s_ftms_svc.discover(conn_handle) || !s_tm_data.discover()) {
        Console.println("[ftms] FTMS Treadmill Data not found, disconnecting");
        conn->disconnect();
        return;
    }

    if (!s_tm_data.enableNotify()) {
        Console.println("[ftms] Failed to subscribe to treadmill data");
        conn->disconnect();
        return;
    }

    if (s_ctrl_point.discover() && s_ctrl_point.enableIndicate()) {
        // Request control of the treadmill (response arrives as an indication)
        uint8_t cmd = FTMS_CP_REQUEST_CONTROL;
        s_ctrl_point.write_resp(&cmd, 1);
    } else {
        Console.println("[ftms] No control point — monitoring only");
    }

    s_had_working_connection = true;
    s_reconnect_delay_ms = RECONNECT_FAST_MS;
    s_reconnect_attempts = 0;
    set_state(FTMS_STATE_STREAMING);
    Console.println("[ftms] Streaming treadmill data");
    if (s_conn_cb) s_conn_cb(true);
}

static void on_central_disconnect(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    Console.printf("[ftms] Treadmill disconnected (reason 0x%02X)\n", reason);
    s_conn_handle = BLE_CONN_HANDLE_INVALID;
    s_control_acquired = false;

    if (s_should_reconnect && s_have_target) {
        set_state(FTMS_STATE_RECONNECTING);
        // Quick retry after a real drop, back off after failures
        s_next_scan_ms = millis() + (s_had_working_connection ? RECONNECT_FAST_MS : s_reconnect_delay_ms);
        s_had_working_connection = false;
    } else {
        set_state(FTMS_STATE_IDLE);
    }
    if (s_conn_cb) s_conn_cb(false);
}

void ftms_client_init(ftms_data_cb_t data_cb, ftms_conn_cb_t conn_cb)
{
    s_data_cb = data_cb;
    s_conn_cb = conn_cb;

    s_ftms_svc.begin();
    s_tm_data.setNotifyCallback(on_tm_data);
    s_tm_data.begin();
    s_ctrl_point.setIndicateCallback(on_ctrl_indicate);
    s_ctrl_point.begin();

    Bluefruit.Central.setConnectCallback(on_central_connect);
    Bluefruit.Central.setDisconnectCallback(on_central_disconnect);

    Bluefruit.Scanner.setRxCallback(on_scan_report);
    Bluefruit.Scanner.setIntervalMS(160, 80);   // 50% duty — leave airtime for the Garmin link
    Bluefruit.Scanner.useActiveScan(true);      // scan responses carry the device name
}

static void begin_list_scan(uint32_t duration_ms);

static void start_scan(uint16_t timeout_10ms)
{
    Bluefruit.Scanner.stop();
    if (!Bluefruit.Scanner.start(timeout_10ms)) {
        Console.println("[ftms] Scan start failed");
    }
}

void ftms_client_update(void)
{
    uint32_t now = millis();

    // Scan finished?
    if (s_user_scan && (int32_t)(now - s_user_scan_end_ms) >= 0) {
        s_user_scan = false;
        Bluefruit.Scanner.stop();
        Console.printf("[ftms] Scan complete: %u device(s)\n", s_scan_count);

        if (s_auto_pick) {
            // Strongest signal = the treadmill we're plugged into
            int best = -1;
            for (uint8_t i = 0; i < s_scan_count; i++) {
                if (best < 0 || s_scan_results[i].rssi > s_scan_results[best].rssi) best = i;
            }
            if (best >= 0) {
                const ftms_scan_result_t *r = &s_scan_results[best];
                Console.printf("[ftms] Nearest treadmill: %s (%d dBm)\n",
                              r->name[0] ? r->name : "(unnamed)", r->rssi);
                s_auto_pick = false;
                ftms_client_connect(r->addr, r->addr_type);
                return;
            }
            Console.println("[ftms] No treadmill found, retrying...");
            s_auto_retry_ms = now + AUTO_RETRY_MS;
            return;
        }

        set_state(s_should_reconnect && s_have_target && !ftms_client_is_connected()
                  ? FTMS_STATE_RECONNECTING : FTMS_STATE_IDLE);
        if (s_state == FTMS_STATE_RECONNECTING) s_next_scan_ms = now;
    }

    // Auto mode: keep searching until a treadmill shows up
    if (s_auto_pick && !s_user_scan && (int32_t)(now - s_auto_retry_ms) >= 0) {
        begin_list_scan(SCAN_AUTO_MS);
    }

    // Connect attempt hung (peer vanished): cancel and fall back to scanning
    if (s_state == FTMS_STATE_CONNECTING && now - s_state_since_ms > CONNECT_TIMEOUT_MS + 2000) {
        sd_ble_gap_connect_cancel();
        Console.println("[ftms] Connect timed out");
        set_state(FTMS_STATE_RECONNECTING);
        s_next_scan_ms = now + s_reconnect_delay_ms;
        s_reconnect_delay_ms = min<uint32_t>(s_reconnect_delay_ms * 2, (uint32_t)RECONNECT_MAX_MS);
        s_reconnect_attempts++;
    }

    // Stuck in discovery
    if (s_state == FTMS_STATE_DISCOVERING && now - s_state_since_ms > 10000) {
        Console.println("[ftms] Discovery timed out, disconnecting");
        if (s_conn_handle != BLE_CONN_HANDLE_INVALID) Bluefruit.Connection(s_conn_handle)->disconnect();
        s_state_since_ms = now;
    }

    // Reconnect: scan until the saved treadmill shows up
    if (s_state == FTMS_STATE_RECONNECTING && !s_user_scan && (int32_t)(now - s_next_scan_ms) >= 0) {
        Console.printf("[ftms] Looking for treadmill (attempt %u)...\n", s_reconnect_attempts + 1);
        s_reconnect_attempts++;
        // Scan for a bounded window; if the treadmill isn't seen we back off and retry
        start_scan(1500);     // 15 s
        s_next_scan_ms = now + 15000 + s_reconnect_delay_ms;
        s_reconnect_delay_ms = min<uint32_t>(s_reconnect_delay_ms * 2, (uint32_t)RECONNECT_MAX_MS);
    }
}

static void begin_list_scan(uint32_t duration_ms)
{
    s_scan_count = 0;
    s_user_scan = true;
    s_user_scan_end_ms = millis() + duration_ms;
    set_state(FTMS_STATE_SCANNING);
    start_scan(0);
}

void ftms_client_scan_start(void)
{
    if (ftms_client_is_connected()) {
        Console.println("[ftms] Already connected — disconnect first to scan");
        return;
    }
    s_auto_pick = false;
    begin_list_scan(SCAN_USER_MS);
    Console.println("[ftms] Scanning for FTMS treadmills (10 s)...");
}

void ftms_client_connect_nearest(void)
{
    s_auto_pick = true;
    s_should_reconnect = false;
    begin_list_scan(SCAN_AUTO_MS);
    Console.println("[ftms] Looking for the nearest treadmill...");
}

void ftms_client_scan_stop(void)
{
    s_user_scan_end_ms = millis();   // let update() wind it down
}

const ftms_scan_result_t *ftms_client_get_scan_results(uint8_t *count)
{
    *count = s_scan_count;
    return s_scan_results;
}

void ftms_client_connect(const uint8_t addr[6], uint8_t addr_type)
{
    memcpy(s_target_addr, addr, 6);
    s_target_addr_type = addr_type;
    s_have_target = true;
    s_should_reconnect = true;
    s_user_scan = false;
    s_reconnect_delay_ms = RECONNECT_FAST_MS;
    s_reconnect_attempts = 0;

    if (ftms_client_is_connected()) return;
    Console.printf("[ftms] Connecting to %02X:%02X:%02X:%02X:%02X:%02X...\n",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    set_state(FTMS_STATE_RECONNECTING);
    s_next_scan_ms = millis();
}

void ftms_client_disconnect(void)
{
    s_should_reconnect = false;
    s_auto_pick = false;
    s_user_scan = false;
    Bluefruit.Scanner.stop();
    if (s_state == FTMS_STATE_CONNECTING) sd_ble_gap_connect_cancel();
    if (s_conn_handle != BLE_CONN_HANDLE_INVALID) {
        Bluefruit.Connection(s_conn_handle)->disconnect();
    }
    set_state(FTMS_STATE_IDLE);
}

bool ftms_client_is_connected(void)
{
    return s_state == FTMS_STATE_DISCOVERING || s_state == FTMS_STATE_STREAMING;
}

ftms_state_t ftms_client_get_state(void)
{
    return s_state;
}

uint16_t ftms_client_get_reconnect_attempts(void)
{
    return s_reconnect_attempts;
}

void ftms_client_set_raw_log(bool enable)
{
    s_raw_log = enable;
}

bool ftms_client_has_control(void)
{
    return s_control_acquired;
}

static bool cp_write(const uint8_t *cmd, uint16_t len)
{
    if (!s_control_acquired || s_state != FTMS_STATE_STREAMING) {
        Console.println("[ftms] No control of treadmill");
        return false;
    }
    return s_ctrl_point.write_resp(cmd, len) == len;
}

bool ftms_client_set_target_speed(uint16_t speed_001kmh)
{
    uint8_t cmd[3] = {FTMS_CP_SET_TARGET_SPEED, (uint8_t)(speed_001kmh & 0xFF), (uint8_t)(speed_001kmh >> 8)};
    return cp_write(cmd, sizeof(cmd));
}

bool ftms_client_set_target_incline(int16_t incline_01pct)
{
    uint8_t cmd[3] = {FTMS_CP_SET_TARGET_INCLINE, (uint8_t)(incline_01pct & 0xFF), (uint8_t)((uint16_t)incline_01pct >> 8)};
    return cp_write(cmd, sizeof(cmd));
}

bool ftms_client_start_treadmill(void)
{
    uint8_t cmd = FTMS_CP_START_RESUME;
    return cp_write(&cmd, 1);
}

bool ftms_client_stop_treadmill(void)
{
    uint8_t cmd[2] = {FTMS_CP_STOP_PAUSE, 0x01};   // 0x01 = Stop
    return cp_write(cmd, sizeof(cmd));
}
