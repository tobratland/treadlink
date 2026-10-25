#include "led_status.h"
#include <Arduino.h>

// XIAO nRF52840 has an RGB LED, active low. Use the blue channel.
#define LED_PIN LED_BLUE

static led_state_t s_state = LED_STATE_NO_CONNECTION;

static void led_write(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

void led_status_init(void)
{
    // Turn the other two channels off
    pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);
    pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
    pinMode(LED_PIN, OUTPUT);
    led_write(false);
}

void led_status_set(led_state_t state)
{
    s_state = state;
}

void led_status_update(void)
{
    uint32_t t = millis();
    bool on = false;

    switch (s_state) {
    case LED_STATE_NO_CONNECTION: on = (t % 2000) < 1000; break;  // 1s on / 1s off
    case LED_STATE_SCANNING:      on = (t % 200) < 100;   break;  // 100ms on / off
    case LED_STATE_TREADMILL_ONLY: {                              // on-off-on then long off
        uint32_t p = t % 1800;
        on = p < 200 || (p >= 400 && p < 600);
        break;
    }
    case LED_STATE_BOTH_CONNECTED: on = true; break;
    }
    led_write(on);
}
