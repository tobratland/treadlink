#pragma once

enum led_state_t {
    LED_STATE_NO_CONNECTION = 0,  // slow blink
    LED_STATE_SCANNING,           // fast blink
    LED_STATE_TREADMILL_ONLY,     // double blink
    LED_STATE_BOTH_CONNECTED,     // solid
};

void led_status_init(void);
void led_status_set(led_state_t state);
void led_status_update(void);   // call from loop()
