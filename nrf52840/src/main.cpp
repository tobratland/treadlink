#include <Arduino.h>
#include <bluefruit.h>

#include "config_store.h"
#include "led_status.h"
#include "data_bridge.h"
#include "ftms_client.h"
#include "rsc_server.h"
#include "console.h"

static treadlink_config_t s_config;

// Simulation: feeds the Garmin link without a treadmill (diagnostics)
static bool     s_sim_active;
static float    s_sim_kmh;
static uint32_t s_sim_last_ms;

// --- Data path ---

static void on_treadmill_data(const ftms_treadmill_data_t *ftms)
{
    if (s_sim_active) return;
    rsc_data_t rsc = data_bridge_convert(ftms);
    rsc_server_notify(&rsc);
}

static void on_connection_change(bool connected)
{
    (void)connected;
}

static void update_led_state(void)
{
    bool tm = ftms_client_is_connected();
    bool gm = rsc_server_is_connected();
    ftms_state_t st = ftms_client_get_state();

    if (tm && gm)                         led_status_set(LED_STATE_BOTH_CONNECTED);
    else if (tm)                          led_status_set(LED_STATE_TREADMILL_ONLY);
    else if (st == FTMS_STATE_SCANNING ||
             st == FTMS_STATE_CONNECTING ||
             st == FTMS_STATE_RECONNECTING) led_status_set(LED_STATE_SCANNING);
    else                                  led_status_set(LED_STATE_NO_CONNECTION);
}

static void sim_update(void)
{
    if (!s_sim_active || millis() - s_sim_last_ms < 1000) return;
    s_sim_last_ms = millis();
    ftms_treadmill_data_t td = {};
    td.speed_001kmh = (uint16_t)(s_sim_kmh * 100.0f);
    rsc_data_t rsc = data_bridge_convert(&td);
    rsc_server_notify(&rsc);
}

// --- Serial CLI ---

static const char *state_name(ftms_state_t s)
{
    switch (s) {
    case FTMS_STATE_IDLE:         return "idle";
    case FTMS_STATE_SCANNING:     return "scanning";
    case FTMS_STATE_CONNECTING:   return "connecting";
    case FTMS_STATE_DISCOVERING:  return "discovering";
    case FTMS_STATE_STREAMING:    return "streaming";
    case FTMS_STATE_RECONNECTING: return "reconnecting";
    }
    return "?";
}

static void print_addr(const uint8_t a[6])
{
    Console.printf("%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static bool parse_addr(const char *s, uint8_t out[6])
{
    unsigned int a[6];
    if (sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)a[5 - i];
    return true;
}

static void apply_config(void)
{
    data_bridge_set_params(s_config.cadence_factor, s_config.cadence_offset, s_config.speed_is_mph,
                           s_config.send_cadence);
}

static void save_config(void)
{
    apply_config();
    Console.println(config_store_save(&s_config) ? "ok (saved)" : "error: save failed");
}

static void cmd_help(void)
{
    Console.println(
        "TreadLink commands:\n"
        "  status                 show connection state and config\n"
        "  scan                   scan for FTMS treadmills (10 s)\n"
        "  list                   show scan results\n"
        "  connect <n|AA:BB:..>   connect to scan result n or an address, and pin it\n"
        "  nearest                connect to the strongest-signal treadmill (default on boot)\n"
        "  disconnect             disconnect and stop auto-reconnect\n"
        "  forget                 unpin the treadmill (boot uses 'nearest' again)\n"
        "  auto on|off            connect automatically on boot (pinned or nearest)\n"
        "  cadence on|off         send estimated cadence (off: watch gets 0)\n"
        "  factor <x>             estimated steps/min = speed_kmh * factor + offset\n"
        "  offset <x>\n"
        "  mph on|off             treadmill reports mph instead of km/h\n"
        "  speed <kmh>            set treadmill target speed\n"
        "  incline <pct>          set treadmill target incline\n"
        "  start | stop           start/stop the treadmill\n"
        "  sim <kmh>|off          feed the watch simulated data\n"
        "  raw on|off             log raw treadmill data packets\n"
        "  pin <6 digits>         set the phone pairing PIN\n"
        "  unpair                 forget all paired phones/watches");
}

static void cmd_status(void)
{
    Console.printf("Treadmill : %s%s\n", state_name(ftms_client_get_state()),
                  ftms_client_has_control() ? " (control)" : "");
    Console.printf("Garmin    : %s\n", rsc_server_is_connected() ? "connected" : "advertising");
    Console.print("Saved     : ");
    if (s_config.has_treadmill) {
        print_addr(s_config.treadmill_addr);
        Console.printf(" %s%s\n", s_config.treadmill_name, s_config.auto_connect ? " [pinned]" : " [pinned, auto off]");
    } else {
        Console.println("none (auto: nearest treadmill)");
    }
    Console.printf("Speed     : %.1f km/h%s\n", data_bridge_get_display_speed_kmh(), s_sim_active ? " (simulated)" : "");
    Console.printf("Cadence   : %s (factor=%.2f offset=%.1f)  mph=%d\n",
                  s_config.send_cadence ? "on" : "off",
                  s_config.cadence_factor, s_config.cadence_offset, s_config.speed_is_mph);
}

static void cmd_list(void)
{
    uint8_t n;
    const ftms_scan_result_t *r = ftms_client_get_scan_results(&n);
    if (n == 0) { Console.println("no devices (run 'scan')"); return; }
    for (uint8_t i = 0; i < n; i++) {
        Console.printf("  %u: ", i + 1);
        print_addr(r[i].addr);
        Console.printf("  %4d dBm  %s\n", r[i].rssi, r[i].name[0] ? r[i].name : "(unnamed)");
    }
}

static void cmd_connect(const char *arg)
{
    uint8_t addr[6], type = 0;
    char name[sizeof(s_config.treadmill_name)] = {0};

    if (strchr(arg, ':')) {
        if (!parse_addr(arg, addr)) { Console.println("error: bad address"); return; }
    } else {
        uint8_t n;
        const ftms_scan_result_t *r = ftms_client_get_scan_results(&n);
        int idx = atoi(arg) - 1;
        if (idx < 0 || idx >= n) { Console.println("error: no such scan result"); return; }
        memcpy(addr, r[idx].addr, 6);
        type = r[idx].addr_type;
        strncpy(name, r[idx].name, sizeof(name) - 1);
    }

    s_config.has_treadmill = true;
    memcpy(s_config.treadmill_addr, addr, 6);
    s_config.treadmill_addr_type = type;
    strncpy(s_config.treadmill_name, name, sizeof(s_config.treadmill_name) - 1);
    s_config.auto_connect = true;
    config_store_save(&s_config);
    ftms_client_connect(addr, type);
}

static void handle_command(char *line)
{
    char *cmd = strtok(line, " \t");
    char *arg = strtok(NULL, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "status"))     cmd_status();
    else if (!strcmp(cmd, "scan"))       ftms_client_scan_start();
    else if (!strcmp(cmd, "list"))       cmd_list();
    else if (!strcmp(cmd, "connect") && arg) cmd_connect(arg);
    else if (!strcmp(cmd, "nearest"))    { ftms_client_disconnect(); ftms_client_connect_nearest(); }
    else if (!strcmp(cmd, "disconnect")) { ftms_client_disconnect(); Console.println("ok"); }
    else if (!strcmp(cmd, "forget")) {
        ftms_client_disconnect();
        s_config.has_treadmill = false;
        memset(s_config.treadmill_addr, 0, 6);
        s_config.treadmill_name[0] = '\0';
        save_config();
    }
    else if (!strcmp(cmd, "auto") && arg)   { s_config.auto_connect = !strcmp(arg, "on"); save_config(); }
    else if (!strcmp(cmd, "cadence") && arg) { s_config.send_cadence = !strcmp(arg, "on"); save_config(); }
    else if (!strcmp(cmd, "factor") && arg) { s_config.cadence_factor = atof(arg); save_config(); }
    else if (!strcmp(cmd, "offset") && arg) { s_config.cadence_offset = atof(arg); save_config(); }
    else if (!strcmp(cmd, "mph") && arg)    { s_config.speed_is_mph = !strcmp(arg, "on"); save_config(); }
    else if (!strcmp(cmd, "speed") && arg)  Console.println(ftms_client_set_target_speed((uint16_t)(atof(arg) * 100.0f)) ? "ok" : "failed");
    else if (!strcmp(cmd, "incline") && arg) Console.println(ftms_client_set_target_incline((int16_t)(atof(arg) * 10.0f)) ? "ok" : "failed");
    else if (!strcmp(cmd, "start"))      Console.println(ftms_client_start_treadmill() ? "ok" : "failed");
    else if (!strcmp(cmd, "stop"))       Console.println(ftms_client_stop_treadmill() ? "ok" : "failed");
    else if (!strcmp(cmd, "sim") && arg) {
        if (!strcmp(arg, "off")) { s_sim_active = false; Console.println("sim off"); }
        else { s_sim_kmh = atof(arg); s_sim_active = true; Console.printf("sim %.1f km/h\n", s_sim_kmh); }
    }
    else if (!strcmp(cmd, "raw") && arg)    { ftms_client_set_raw_log(!strcmp(arg, "on")); Console.println("ok"); }
    else if (!strcmp(cmd, "pin") && arg) {
        if (strlen(arg) != 6 || strspn(arg, "0123456789") != 6) { Console.println("error: PIN must be 6 digits"); return; }
        strcpy(s_config.ble_pin, arg);
        console_set_pin(arg);
        save_config();
    }
    else if (!strcmp(cmd, "unpair")) {
        Bluefruit.Periph.clearBonds();
        Console.println("ok (bonds cleared; re-pair the phone and watch)");
    }
    else Console.println("unknown command — try 'help'");
}

// --- Main ---

void setup()
{
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) delay(10);
    Serial.println("TreadLink (nRF52840) starting...");

    config_store_init();
    config_store_load(&s_config);
    apply_config();

    led_status_init();
    led_status_set(LED_STATE_NO_CONNECTION);

    // 2 peripheral links (Garmin + phone console) + 1 central link (treadmill)
    Bluefruit.autoConnLed(false);
    Bluefruit.begin(2, 1);
    Bluefruit.setName("TreadLink");
    Bluefruit.setTxPower(4);

    rsc_server_init(on_connection_change);
    ftms_client_init(on_treadmill_data, on_connection_change);
    console_init(s_config.ble_pin);
    rsc_server_start_advertising();

    // Auto-connect to saved treadmill
    if (s_config.auto_connect && s_config.has_treadmill) {
        Console.print("Auto-connecting to ");
        print_addr(s_config.treadmill_addr);
        Console.println();
        ftms_client_connect(s_config.treadmill_addr, s_config.treadmill_addr_type);
    } else if (s_config.auto_connect) {
        // No pinned treadmill: grab whichever one we're plugged into
        ftms_client_connect_nearest();
    }

    // Hardware watchdog: reboot if loop() ever stalls
    NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
    NRF_WDT->CRV = 10 * 32768;
    NRF_WDT->RREN = WDT_RREN_RR0_Enabled;
    NRF_WDT->TASKS_START = 1;
}

void loop()
{
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;

    console_poll(handle_command);
    ftms_client_update();
    sim_update();
    update_led_state();
    led_status_update();
    delay(20);
}
