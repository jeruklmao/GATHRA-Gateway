# Development-session validation results

This records reproducible evidence from the 2026-08-18 implementation session.
It distinguishes host/build validation from observations that require the
physical Gateway or production Node.

## Automated and build results

| Check | Result | Evidence |
| --- | --- | --- |
| Native firmware tests | PASS | 12/12 Protocol, pairing, dedup, queue, ACK-order, configuration, and Backend-contract cases |
| Production build | PASS | 1,160,888/1,441,792 program bytes (80.5%); 67,116/327,680 RAM bytes (20.5%) |
| Production image | PASS | Valid ESP32-C3 image; `firmware.bin` is 1,217,440 bytes with checksum and validation hash |
| HIL profile build | PASS | Synthetic post-RX support compiled only with `GATHRA_HIL_SYNTHETIC` |
| Rollback-test profile build | PASS | Controlled pending-image failure profile compiled separately |
| Partition table | PASS | Two 1,408 KiB OTA slots, 1,152 KiB LittleFS, 20 KiB NVS, 8 KiB OTA data, 64 KiB coredump; ends at 4 MiB |
| Queue capacity model | PASS | 3,182 conservative maximum-sized records from the 1,152 KiB filesystem budget |

The PlatformIO board target reports 4 MiB flash. Physical flash-size probing is
not recorded as a pass because the Gateway did not enumerate on USB during the
final bench checks.

## Hardware visibility during this session

Repeated `pio device list`, `/dev/ttyACM*`/`/dev/ttyUSB*`, and `lsusb` checks
found no ESP32-C3 or USB-serial device. Consequently, flashing and serial boot
capture were blocked before permissions or firmware could be involved. The
dedicated Realtek RTL8188EUS adapter (`0bda:8179`, `rtl8xxxu`) was visible and
scanned explicitly, but no `GATHRA-GW-*` AP existed because the Gateway had not
booted this image. The host's Intel adapter was not reconfigured.

| Hardware/HIL observation | Result |
| --- | --- |
| ESP32-C3 boot and physical flash probe | BLOCKED — device absent from USB enumeration |
| SX1278 initialization and continuous RX | BLOCKED — no serial/USB access to Gateway |
| Fallback AP, dashboard, provisioning, queue reboot recovery | BLOCKED — firmware could not be flashed |
| Synthetic real-radio ACK TX, OTA, bootloader rollback | BLOCKED — firmware could not be flashed |
| Real Node telemetry reception | NOT TESTED — Node deployed in field |
| Node receives Gateway ACK | NOT TESTED — Node deployed in field |
| Node retry after lost ACK | NOT TESTED — Node deployed in field |
| End-to-end RF ACK latency | NOT TESTED — Node deployed in field |
| Real Node RSSI/SNR | NOT TESTED — Node deployed in field |

After USB enumeration is restored, follow `docs/testing.md` for Gateway HIL and
`docs/pending-node-rf-validation.md` when the production Node is available.
