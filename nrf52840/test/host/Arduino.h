// Host-test stub for the bits of Arduino the portable modules use
#pragma once
#include <stdint.h>
extern uint32_t g_fake_millis;
static inline uint32_t millis(void) { return g_fake_millis; }
