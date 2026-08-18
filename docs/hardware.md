# Hardware

Gateway v1 contains only an ESP32-C3 Super Mini and SX1278 RA-02. It requires
continuous external power and has no sensors, battery measurement, button, or
deep-sleep lifecycle.

## Immutable wiring

`include/board_pins.hpp` is the single source of truth. Dashboard and NVS
configuration cannot override it.

| ESP32-C3 | SX1278 RA-02 |
| ---: | --- |
| GPIO1 | RST |
| GPIO3 | DIO0 |
| GPIO4 | SCK |
| GPIO5 | MISO |
| GPIO6 | MOSI |
| GPIO7 | NSS / CS |

This is intentionally identical to the radio portion of the production Node.
Do not move pins in firmware to mask an initialization fault. Verify 3.3 V
power, common ground, SPI continuity, reset, DIO0, antenna, and the 433 MHz
module variant if RadioLib initialization fails.

## Default radio parameters

| Parameter | Default |
| --- | ---: |
| Frequency | 433.0 MHz |
| Bandwidth | 125 kHz |
| Spreading factor | 10 |
| Coding rate | 4/6 |
| TX power | 17 dBm |
| Sync word | `0x12` |
| Packet CRC | enabled |

All values except pins and CRC policy are dashboard-editable with validation
and apply/rollback behavior.

## Flash

The PlatformIO target and partition table assume the detected/expected 4 MiB
embedded flash. No partition extends past `0x400000`. Do not flash this table to
a smaller device.
