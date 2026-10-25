#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "data_bridge.h"

#define FTMS_MAX_SCAN_RESULTS 10

enum ftms_state_t {
    FTMS_STATE_IDLE = 0,
    FTMS_STATE_SCANNING,
    FTMS_STATE_CONNECTING,
    FTMS_STATE_DISCOVERING,
    FTMS_STATE_STREAMING,
    FTMS_STATE_RECONNECTING,   // scanning for the saved treadmill after a drop
};

struct ftms_scan_result_t {
    uint8_t addr[6];           // little-endian, as the SoftDevice reports it
    uint8_t addr_type;
    char    name[32];
    int8_t  rssi;
};

typedef void (*ftms_data_cb_t)(const ftms_treadmill_data_t *data);
typedef void (*ftms_conn_cb_t)(bool connected);

void ftms_client_init(ftms_data_cb_t data_cb, ftms_conn_cb_t conn_cb);  // after Bluefruit.begin()
void ftms_client_update(void);                                          // call from loop()

// User scan: lists nearby FTMS devices for ~10 s
void ftms_client_scan_start(void);
void ftms_client_scan_stop(void);
const ftms_scan_result_t *ftms_client_get_scan_results(uint8_t *count);

// Connect to a treadmill and keep reconnecting to it until disconnect() is called
void ftms_client_connect(const uint8_t addr[6], uint8_t addr_type);
void ftms_client_disconnect(void);

// Scan briefly, connect to the strongest-signal treadmill (not saved); retries until one is found
void ftms_client_connect_nearest(void);

bool ftms_client_is_connected(void);
ftms_state_t ftms_client_get_state(void);
uint16_t ftms_client_get_reconnect_attempts(void);
void ftms_client_set_raw_log(bool enable);   // hex-dump every treadmill data packet

// FTMS Control Point — treadmill control
bool ftms_client_has_control(void);
bool ftms_client_set_target_speed(uint16_t speed_001kmh);
bool ftms_client_set_target_incline(int16_t incline_01pct);
bool ftms_client_start_treadmill(void);
bool ftms_client_stop_treadmill(void);
