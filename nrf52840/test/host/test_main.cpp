// Host-side tests for the FTMS parser and data bridge.
// Run: g++ -std=c++17 -I. -I../../src test_main.cpp ../../src/ftms_parse.cpp ../../src/data_bridge.cpp -o /tmp/t && /tmp/t
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ftms_parse.h"
#include "data_bridge.h"

uint32_t g_fake_millis = 1000;

static double pace_min_per_km(uint16_t speed_256ths)
{
    double mps = speed_256ths / 256.0;
    return 1000.0 / mps / 60.0;
}

int main()
{
    ftms_treadmill_data_t td = {};

    // Packet A: speed 8.00 km/h (800 = 0x0320), total distance 1234 m — More Data clear
    const uint8_t a[] = {0x04, 0x00, 0x20, 0x03, 0xD2, 0x04, 0x00};
    assert(ftms_parse_treadmill_data(a, sizeof(a), &td));
    assert(td.speed_001kmh == 800 && td.total_distance_m == 1234);

    // Packet B (Woodway-style split): More Data SET, average speed 7.80 km/h + incline 1.5%.
    // Speed must NOT change — this was the 7:30 -> 7:40 flicker.
    const uint8_t b[] = {0x0B, 0x00, 0x0C, 0x03, 0x0F, 0x00, 0x00, 0x00};
    assert(ftms_parse_treadmill_data(b, sizeof(b), &td));
    assert(td.speed_001kmh == 800);
    assert(td.has_incline && td.incline_01pct == 15);
    assert(td.total_distance_m == 1234);

    // Paused: speed 0 in packet A form, then a More Data packet with stale average speed
    const uint8_t a0[] = {0x00, 0x00, 0x00, 0x00};
    assert(ftms_parse_treadmill_data(a0, sizeof(a0), &td));
    assert(ftms_parse_treadmill_data(b, sizeof(b), &td));
    assert(td.speed_001kmh == 0);

    // Truncated packet is rejected and leaves state untouched
    const uint8_t bad[] = {0x04, 0x00, 0x20, 0x03, 0xD2};
    assert(!ftms_parse_treadmill_data(bad, sizeof(bad), &td));
    assert(td.speed_001kmh == 0);

    // Bridge: 8.00 km/h must be 7:30 /km, cadence off by default -> 0
    data_bridge_set_params(1.4f, 120.0f, false, false);
    ftms_treadmill_data_t in = {};
    in.speed_001kmh = 800;
    rsc_data_t r = data_bridge_convert(&in);
    double pace = pace_min_per_km(r.speed_256ths_mps);
    printf("8.00 km/h -> %u/256 m/s -> pace %d:%04.1f, cadence %u\n", r.speed_256ths_mps,
           (int)pace, fmod(pace, 1.0) * 60.0, r.cadence_spm);
    assert(fabs(pace - 7.5) < 0.01);
    assert(r.cadence_spm == 0);

    // Every treadmill speed from 1.0 to 25.0 km/h: the speed we send must never be slower
    // than the treadmill's (watch rounds pace up to 5 s), and less than one 1/256 m/s step faster.
    data_bridge_set_params(1.4f, 120.0f, false, false);
    for (uint16_t s = 100; s <= 2500; s += 10) {
        ftms_treadmill_data_t t = {};
        t.speed_001kmh = s;
        rsc_data_t o = data_bridge_convert(&t);
        double true_steps = s / 100.0 / 3.6 * 256.0;   // exact speed in 1/256 m/s
        double diff = o.speed_256ths_mps - true_steps;
        if (!(diff > -1e-6 && diff < 1.0)) {
            printf("FAIL at %.1f km/h: true %.4f steps, sent %u\n", s / 100.0, true_steps, o.speed_256ths_mps);
            return 1;
        }
    }
    in.speed_001kmh = 1000;   // the 6:00 -> 6:05 case
    r = data_bridge_convert(&in);
    printf("10.0 km/h -> %u/256 m/s -> %.2f s/km (true 360.00)\n", r.speed_256ths_mps, 1000.0 / (r.speed_256ths_mps / 256.0));
    in.speed_001kmh = 800;

    // Cadence on: 8 km/h * 1.4 + 120 = 131.2 steps/min -> 66 strides/min sent
    data_bridge_set_params(1.4f, 120.0f, false, true);
    r = data_bridge_convert(&in);
    assert(r.cadence_spm == 66);

    puts("all tests passed");
    return 0;
}
