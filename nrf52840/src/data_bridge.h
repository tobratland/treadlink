#pragma once

#include <stdint.h>
#include <stdbool.h>

// Input from FTMS treadmill
struct ftms_treadmill_data_t {
    uint16_t speed_001kmh;       // Speed in 0.01 km/h (or mph if misconfigured)
    int16_t  incline_01pct;      // Incline in 0.1% (signed)
    uint32_t total_distance_m;   // Total distance in meters
    bool     has_incline;
    bool     has_distance;
};

// Output for RSC (footpod)
struct rsc_data_t {
    uint16_t speed_256ths_mps;   // Speed in m/s * 256
    uint8_t  cadence_spm;        // RSC cadence field: strides/min (watch doubles it to steps); 0 = not sent
    uint16_t stride_length_cm;   // Stride length in cm
    uint32_t total_distance_dm;  // Total distance in decimeters
    bool     is_running;         // Walking vs running flag
};

// cadence_factor/offset give an estimate in steps/min; send_cadence=false reports 0
void data_bridge_set_params(float cadence_factor, float cadence_offset, bool speed_is_mph, bool send_cadence);
rsc_data_t data_bridge_convert(const ftms_treadmill_data_t *ftms);
float data_bridge_get_display_speed_kmh(void);
