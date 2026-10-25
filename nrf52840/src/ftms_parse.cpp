#include "ftms_parse.h"

bool ftms_parse_treadmill_data(const uint8_t *data, uint16_t len, ftms_treadmill_data_t *td)
{
    if (len < 2) return false;

    uint16_t flags = data[0] | (data[1] << 8);
    uint16_t offset = 2;

    // Bit 0 "More Data": when SET, Instantaneous Speed is NOT present in this packet
    bool has_speed = !(flags & (1 << 0));
    uint16_t speed = 0;
    if (has_speed) {
        if (offset + 2 > len) return false;
        speed = data[offset] | (data[offset + 1] << 8);
        offset += 2;
    }

    // Average Speed (bit 1) — skipped
    if (flags & (1 << 1)) {
        if (offset + 2 > len) return false;
        offset += 2;
    }

    // Total Distance (bit 2) - uint24, meters
    uint32_t distance = 0;
    bool has_distance = flags & (1 << 2);
    if (has_distance) {
        if (offset + 3 > len) return false;
        distance = data[offset] | (data[offset + 1] << 8) | ((uint32_t)data[offset + 2] << 16);
        offset += 3;
    }

    // Inclination + Ramp Angle (bit 3) - sint16 + sint16
    int16_t incline = 0;
    bool has_incline = flags & (1 << 3);
    if (has_incline) {
        if (offset + 4 > len) return false;
        incline = (int16_t)(data[offset] | (data[offset + 1] << 8));
        offset += 4;
    }

    // Packet fully validated: commit only the fields it carried
    if (has_speed) td->speed_001kmh = speed;
    if (has_distance) { td->total_distance_m = distance; td->has_distance = true; }
    if (has_incline) { td->incline_01pct = incline; td->has_incline = true; }
    return true;
}
