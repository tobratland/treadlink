#include "data_bridge.h"
#include <Arduino.h>
#include <math.h>

static float s_cadence_factor = 1.4f;
static float s_cadence_offset = 120.0f;
static bool s_speed_is_mph = false;
static bool s_send_cadence = false;

// Fallback distance tracking when treadmill doesn't report odometer
static uint32_t s_fallback_distance_dm;
static uint32_t s_last_update_ms;

// Last converted speed for UI
static float s_display_speed_kmh;

#define RUNNING_THRESHOLD_KMH 6.0f
#define MPH_TO_KMH 1.60934f

void data_bridge_set_params(float cadence_factor, float cadence_offset, bool speed_is_mph, bool send_cadence)
{
    s_send_cadence = send_cadence;
    s_cadence_factor = cadence_factor;
    s_cadence_offset = cadence_offset;
    s_speed_is_mph = speed_is_mph;
}

rsc_data_t data_bridge_convert(const ftms_treadmill_data_t *ftms)
{
    rsc_data_t rsc = {};

    // Convert speed — apply mph correction if needed
    float speed_kmh = ftms->speed_001kmh / 100.0f;
    if (s_speed_is_mph) {
        speed_kmh *= MPH_TO_KMH;
    }
    s_display_speed_kmh = speed_kmh;

    float speed_mps = speed_kmh / 3.6f;
    // Round UP to the next 1/256 m/s step: Garmin shows pace rounded up to 5 s, so reporting
    // even a hair slow turns e.g. a true 6:00/km (10.0 km/h) into 6:05 on the watch.
    // The epsilon keeps exact steps (and float noise just above them) from bumping a step.
    rsc.speed_256ths_mps = (uint16_t)ceilf(speed_mps * 256.0f - 0.001f);

    // Walking vs running (use corrected speed)
    rsc.is_running = (speed_kmh > RUNNING_THRESHOLD_KMH);

    // Optional cadence estimate (steps/min) from corrected speed. RSC's cadence field is
    // strides/min — the watch doubles it — so halve before sending.
    if (s_send_cadence && speed_kmh > 0.1f) {
        float steps = speed_kmh * s_cadence_factor + s_cadence_offset;
        if (steps < 0) steps = 0;
        if (steps > 508) steps = 508;
        rsc.cadence_spm = (uint8_t)(steps / 2.0f + 0.5f);
        float stride_m = speed_mps / (steps / 60.0f);
        rsc.stride_length_cm = (uint16_t)(stride_m * 100.0f);
    }

    // Distance: prefer treadmill odometer, fall back to speed integration
    if (ftms->has_distance && ftms->total_distance_m > 0) {
        rsc.total_distance_dm = ftms->total_distance_m * 10;
        s_fallback_distance_dm = rsc.total_distance_dm; // keep in sync for smooth handoff
    } else {
        uint32_t now = millis();
        if (speed_mps > 0.01f) {
            if (s_last_update_ms > 0) {
                float dt_s = (now - s_last_update_ms) / 1000.0f;
                if (dt_s > 0.0f && dt_s < 5.0f) {
                    s_fallback_distance_dm += (uint32_t)(speed_mps * dt_s * 10.0f);
                }
            }
            s_last_update_ms = now ? now : 1;
        } else {
            s_last_update_ms = 0; // reset timer when stopped
        }
        rsc.total_distance_dm = s_fallback_distance_dm;
    }

    return rsc;
}

float data_bridge_get_display_speed_kmh(void)
{
    return s_display_speed_kmh;
}
