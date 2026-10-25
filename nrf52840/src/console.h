#pragma once

#include <Arduino.h>

// Output goes to USB serial and, once a PIN-paired phone is connected, to BLE UART (NUS).
class ConsoleOut : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t len) override;
    using Print::write;
};

extern ConsoleOut Console;

typedef void (*console_cmd_cb_t)(char *line);

void console_init(const char *pin);          // after Bluefruit.begin(), before advertising
void console_set_pin(const char *pin);
void console_poll(console_cmd_cb_t handler); // call from loop()
bool console_is_phone(uint16_t conn_handle);
void console_add_adv_uuid(void);              // advertises the NUS UUID in the scan response
