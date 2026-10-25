#include "rsc_server.h"
#include <bluefruit.h>
#include "console.h"

// RSC Feature: total distance (bit 1) + walk/run status (bit 2)
#define RSC_FEATURE_FLAGS 0x0006
// Sensor location: Top of shoe
#define SENSOR_LOCATION   0x01

static BLEService        s_rsc_svc(0x1814);
static BLECharacteristic s_rsc_meas(0x2A53);
static BLECharacteristic s_rsc_feature(0x2A54);
static BLECharacteristic s_rsc_location(0x2A5D);
static BLEDis            s_dis;

static rsc_conn_cb_t s_conn_cb;
static volatile bool s_connected;
static volatile uint16_t s_conn_handle = BLE_CONN_HANDLE_INVALID;

#define MAX_PERIPH_LINKS 2   // watch + phone

// Two peripheral links share this callback (watch and phone). The watch is the one that
// subscribes to RSC measurements, so that's what marks a link as "Garmin".
static void on_connect(uint16_t conn_handle)
{
    (void)conn_handle;
    // Advertising stops on connect; keep it going while a slot is free so the other device can join
    if (Bluefruit.Periph.connected() < MAX_PERIPH_LINKS) Bluefruit.Advertising.start(0);
}

static void on_disconnect(uint16_t conn_handle, uint8_t reason)
{
    if (conn_handle == s_conn_handle) {
        s_conn_handle = BLE_CONN_HANDLE_INVALID;
        s_connected = false;
        Console.printf("[rsc] Garmin disconnected (reason 0x%02X)\n", reason);
        if (s_conn_cb) s_conn_cb(false);
    }
    // Advertising restarts automatically (restartOnDisconnect)
}

static void on_cccd(uint16_t conn_handle, BLECharacteristic *chr, uint16_t value)
{
    if (chr->uuid != s_rsc_meas.uuid) return;
    bool enabled = value & 1;
    if (enabled && !s_connected) {
        s_conn_handle = conn_handle;
        s_connected = true;
        Console.println("[rsc] Garmin connected");
        if (s_conn_cb) s_conn_cb(true);
    }
    Console.printf("[rsc] Garmin %s RSC notifications\n", enabled ? "enabled" : "disabled");
}

void rsc_server_init(rsc_conn_cb_t conn_cb)
{
    s_conn_cb = conn_cb;

    Bluefruit.Periph.setConnectCallback(on_connect);
    Bluefruit.Periph.setDisconnectCallback(on_disconnect);

    s_dis.setManufacturer("TreadLink");
    s_dis.setModel("TL-2");
    s_dis.setHardwareRev("XIAO nRF52840");
    s_dis.setSoftwareRev("1.0.0");
    s_dis.begin();

    s_rsc_svc.begin();

    s_rsc_meas.setProperties(CHR_PROPS_NOTIFY);
    s_rsc_meas.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    s_rsc_meas.setFixedLen(8);
    s_rsc_meas.setCccdWriteCallback(on_cccd);
    s_rsc_meas.begin();

    s_rsc_feature.setProperties(CHR_PROPS_READ);
    s_rsc_feature.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    s_rsc_feature.setFixedLen(2);
    s_rsc_feature.begin();
    s_rsc_feature.write16(RSC_FEATURE_FLAGS);

    s_rsc_location.setProperties(CHR_PROPS_READ);
    s_rsc_location.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    s_rsc_location.setFixedLen(1);
    s_rsc_location.begin();
    s_rsc_location.write8(SENSOR_LOCATION);
}

void rsc_server_start_advertising(void)
{
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addAppearance(0x0441);
    Bluefruit.Advertising.addService(s_rsc_svc);
    Bluefruit.ScanResponse.addName();
    console_add_adv_uuid();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(160, 400);   // 100–250 ms in 0.625 ms units
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);
}

bool rsc_server_notify(const rsc_data_t *data)
{
    if (!s_connected || !s_rsc_meas.notifyEnabled(s_conn_handle)) return false;

    uint8_t buf[8];
    uint8_t flags = (1 << 1);                 // total distance present
    if (data->is_running) flags |= (1 << 2);

    buf[0] = flags;
    buf[1] = data->speed_256ths_mps & 0xFF;
    buf[2] = data->speed_256ths_mps >> 8;
    buf[3] = data->cadence_spm;
    buf[4] = data->total_distance_dm & 0xFF;
    buf[5] = (data->total_distance_dm >> 8) & 0xFF;
    buf[6] = (data->total_distance_dm >> 16) & 0xFF;
    buf[7] = (data->total_distance_dm >> 24) & 0xFF;

    return s_rsc_meas.notify(s_conn_handle, buf, sizeof(buf));
}

bool rsc_server_is_connected(void)
{
    return s_connected;
}
