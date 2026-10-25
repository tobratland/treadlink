#pragma once

#include <stdint.h>
#include <stdbool.h>

struct treadlink_config_t {
    uint32_t magic;
    bool     has_treadmill;
    uint8_t  treadmill_addr[6];   // little-endian, as the SoftDevice reports it
    uint8_t  treadmill_addr_type;
    char     treadmill_name[32];
    bool     auto_connect;
    float    cadence_factor;      // speed-to-cadence multiplier
    float    cadence_offset;      // cadence base offset
    bool     speed_is_mph;        // treadmill reports mph instead of km/h
    char     ble_pin[7];          // 6-digit PIN for the phone console
    bool     send_cadence;        // send estimated cadence (off: watch gets 0)
    // New fields go at the end: older saved configs load with these left at defaults
};

void config_store_init(void);
void config_store_defaults(treadlink_config_t *config);
void config_store_load(treadlink_config_t *config);
bool config_store_save(const treadlink_config_t *config);
