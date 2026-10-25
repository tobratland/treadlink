# TreadLink — nRF52840 port

Port of TreadLink to the **Seeed Studio XIAO nRF52840 Plus** (Arduino + Adafruit Bluefruit/SoftDevice S140).

```
Treadmill ──BLE FTMS──> TreadLink ──BLE RSC──> Garmin Watch
                            │
              USB serial / phone (BLE UART) console
```

## Differences from the ESP32-S3 version

The nRF52840 has no WiFi, so the web UI, WiFi AP/STA and REST API are gone. Configuration and
diagnostics move to a console you can reach over USB serial (115200 baud) or from your phone over
Bluetooth (see below). Everything on the BLE side is the same:
FTMS client + RSC server at the same time, speed/cadence/distance conversion, auto-reconnect with
backoff, FTMS control point, simulation, status LED. Settings persist in on-chip flash (LittleFS).

## Build & flash

```bash
cd nrf52840
pio run                    # build
pio run -t upload          # flash over USB (bootloader must be active)
pio device monitor         # serial console
```

If upload can't find the board, double-tap the reset button so the LED pulses (bootloader mode).
Alternatively copy a UF2 to the `XIAO-SENSE` drive that appears.

## Usage

Plug it into the treadmill's USB port. On boot it:

1. Starts advertising as a footpod, so the watch can connect at any time.
2. Scans for 5 s and connects to the FTMS treadmill with the strongest signal (the one it's
   plugged into). If none is found it keeps retrying; if the link drops it reconnects to the same one.

First time only, on the Garmin: Settings > Sensors > Add > search for "TreadLink". The watch
remembers it after that.

## Phone console

Install a BLE terminal app such as **nRF Connect** or **Bluefruit Connect** (iOS/Android), connect to
"TreadLink" and open its UART. The phone asks to pair: enter the PIN **123456** (once; the phone
remembers it). Commands from a phone are ignored until it has paired with the PIN, so nobody else
in the gym can control your treadmill. Change the PIN with `pin <6 digits>`; `unpair` forgets all
paired phones and watches.

## Pinning a treadmill

To always use one specific treadmill instead of the nearest, pin it from the console:

```
scan                 # lists FTMS treadmills for 10 s
list
connect 1            # pin it; 'forget' goes back to nearest
```

Type `help` for the full command list (`status`, `forget`, `mph on|off`, `cadence on|off`,
`speed`, `incline`, `start`, `stop`, `sim <kmh>`, `raw on` to log treadmill packets).

Cadence is not sent by default (the watch gets 0). `cadence on` sends an estimate from speed,
tuned with `factor`/`offset` (steps/min = km/h × factor + offset).

## Speed accuracy

Speed is sent at the footpod format's 1/256 m/s resolution, always rounded up so it is never
slower than the treadmill (at most ~0.3 s/km faster). Garmin watches display live pace rounded up to
5 s steps (e.g. 3:41 shows as 3:45), but the saved activity keeps full precision.

Some treadmills (e.g. Woodway) split FTMS data across notifications using the "More Data" flag;
this is handled. `raw on` logs every packet if a treadmill misbehaves.

## Tests

```bash
g++ -std=c++17 -Itest/host -Isrc test/host/test_main.cpp src/ftms_parse.cpp src/data_bridge.cpp -o /tmp/tl_test && /tmp/tl_test
```

## LED (blue)

| Pattern | Meaning |
|---------|---------|
| Slow blink (1s) | No connection |
| Fast blink (100ms) | Scanning / connecting |
| Double blink | Treadmill connected, waiting for Garmin |
| Solid on | Both connected — data flowing |
