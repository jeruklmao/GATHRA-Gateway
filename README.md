# GATHRA Gateway firmware

Production firmware v1.0.0 for one ESP32-C3 Super Mini plus one SX1278
RA-02. The Gateway is always powered, listens continuously for the one
explicitly paired GATHRA Node, durably spools exact Protocol v1 packets, ACKs
only after local persistence, and uploads asynchronously to the GATHRA Backend.

```text
SX1278 DIO0 -> priority-4 radio worker -> Protocol v1 validation
                                      -> paired-Node enforcement
                                      -> LittleFS durable record
                                      -> immediate ACK and RX restart

LittleFS queue -> priority-1 backend worker -> HTTPS batch ingestion
Wi-Fi/AP       -> local dashboard           -> provisioning/diagnostics/OTA
```

## Product boundaries

- Gateway v1 supports exactly one paired Node.
- LoRa Protocol v1 intentionally has no authentication or HMAC.
- The LAN/fallback-AP dashboard has no application-level authentication in v1.
- The device is AC powered; there is no deep sleep, sensor, button, or battery
  monitoring.
- The production backend default is `https://api.gathra.my.id` with normal TLS
  certificate and hostname validation. Firmware never calls `setInsecure()`.
- Telemetry is not converted into a flood classification or `FloodHazard`.

The fallback WPA2 AP is `GATHRA-GW-<short-mac>` with password
`sman35jakarta`. Its dashboard is normally `http://192.168.4.1/`.

## Build and test

Prerequisites are PlatformIO Core and a host C++ compiler.

```bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
pio run -e rollback-test
pio device list
```

The production environment excludes synthetic injection. The `hil` environment
adds serial-only commands that inject the canonical Node golden packet after
the physical-RX boundary and can transmit its real ACK:

```text
HIL_PAIR
HIL_INJECT 123
HIL_SUPPRESS_ACK_ONCE
HIL_RADIO_CYCLE
HIL_UNPAIR
```

Never deploy `hil` or `rollback-test` as the final image.

## Flash layout

The target is fixed at 4 MiB flash. `partitions.csv` defines two 1,408 KiB OTA
slots, a 1,152 KiB LittleFS spool, NVS, OTA metadata, and a 64 KiB coredump
partition. The queue derives its runtime capacity from the mounted filesystem;
with the full partition and maximum-size framed records, the conservative
build-time estimate is 3,182 records.

The clean production build recorded on 2026-08-18 uses 1,160,888 of the
1,441,792 program bytes reported by PlatformIO (80.5%), uses 67,116 of 327,680
RAM bytes (20.5%), and produces a 1,217,440-byte flashable `firmware.bin`.
These are build-target measurements; physical flash probing is tracked
separately. See
[docs/durable-queue.md](docs/durable-queue.md) for the calculation.

## Documentation

- [Architecture](docs/architecture.md)
- [Immutable hardware wiring](docs/hardware.md)
- [Protocol compatibility](docs/protocol-compatibility.md)
- [Pairing](docs/pairing.md)
- [Wi-Fi provisioning](docs/provisioning.md)
- [Durable queue](docs/durable-queue.md)
- [Backend contract](docs/backend-contract.md)
- [Dashboard](docs/dashboard.md)
- [OTA and rollback](docs/ota.md)
- [Testing and HIL](docs/testing.md)
- [Development-session validation results](docs/validation-results.md)
- [Pending real-Node RF procedure](docs/pending-node-rf-validation.md)
