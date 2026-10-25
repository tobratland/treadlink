#pragma once

#include <stdint.h>
#include "data_bridge.h"

// Parse one FTMS Treadmill Data notification (0x2ACD) into `td`, updating only the fields
// present in this packet. Treadmills may split data across several notifications
// (the "More Data" flag), so `td` should persist between calls. Returns false if malformed.
bool ftms_parse_treadmill_data(const uint8_t *data, uint16_t len, ftms_treadmill_data_t *td);
