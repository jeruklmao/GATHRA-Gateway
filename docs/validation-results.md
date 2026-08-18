# Development-session validation results

This is the observed bench evidence from 2026-08-18. Synthetic telemetry uses
the canonical packet derived from the production Node codec and enters the HIL
firmware immediately after the physical LoRa read boundary. It therefore tests
the production decode, pairing, dedup, durable queue, ACK transmit, Backend
worker, HTTP ingestion, PostgreSQL, and monitoring paths, but it is not evidence
of an over-the-air Node-to-Gateway reception.

## Build and storage

| Check | Result | Evidence |
| --- | --- | --- |
| Native firmware tests | PASS | 12/12 Protocol, pairing, dedup, queue, ACK-order, configuration, and Backend-contract cases |
| Production build | PASS | 1,161,508/1,441,792 linked program bytes (80.6%); 1,218,208-byte `.bin`; 67,116/327,680 static RAM bytes (20.5%) |
| HIL and rollback profiles | PASS | Both isolated profiles compile; injection is absent unless `GATHRA_HIL_SYNTHETIC` is defined |
| Physical flash | PASS | ESP32-C3 rev 0.4 reported 4 MiB XMC embedded flash |
| Partition table | PASS | Two 1,408 KiB OTA slots, 1,152 KiB LittleFS, 20 KiB NVS, 8 KiB OTA data, and 64 KiB coredump; final end is exactly 4 MiB |
| LittleFS | PASS | Physical mount reported 1,179,648 total bytes and 16,384 used bytes after recovery |
| Queue capacity | PASS | Runtime-derived capacity is 3,182 conservative maximum-sized records |

The PlatformIO uploader's flasher stub disconnected on this native USB/JTAG
board. ROM-only flashing at 115200 baud with DIO/80 MHz/4 MiB parameters and
`--after watchdog_reset` wrote and hash-verified the image. A normal USB core
reset was not sufficient to leave download mode; the watchdog reset was.

## Physical Gateway HIL

| Observation | Result | Evidence |
| --- | --- | --- |
| ESP boot | PASS | POWER_ON and SOFTWARE boot logs captured; production and HIL images remained stable |
| SX1278 initialization | PASS | 433.000 MHz, 125 kHz, SF10, CR 4/6, 17 dBm, sync `0x12`, CRC on |
| Continuous RX / RX restoration | PASS | State returned to `RECEIVING` after boot, ACK TX, restart, reconfiguration, standby, and sleep cycle |
| Pairing workflow | PASS | Discovery produced candidate `N1`; no discovery enqueue/ACK; explicit confirmation enabled the next packet; unpair enforcement passed |
| Durable enqueue | PASS | Canonical packets survived LittleFS append and reboot recovery; filesystem error counter remained zero |
| Synthetic ACK TX | PASS | Physical SX1278 completed correctly encoded ACK transmissions and immediately restored RX |
| Duplicate handling | PASS | Exact retry re-ACKed without increasing queue depth |
| Suppressed-ACK retry model | PASS | First packet persisted without ACK; repeated tuple was classified duplicate and re-ACKed |
| Fallback AP | PASS | `GATHRA-GW-D4E958`, WPA2 password `sman35jakarta`, dashboard at `192.168.4.1` |
| STA provisioning / LAN dashboard | PASS | Joined an Intel-hosted isolated AP at `10.42.0.175`; LAN dashboard HTTP 200; AP stopped after the 30-second grace period |
| Extended STA loss fallback | PASS | AP returned after the configured 60-second disconnected interval |
| Runtime credential clearing | PASS | Explicit clear canceled the old ESP driver connection attempt; fallback-AP OTA returned to normal speed |
| Dashboard controls | PASS | Status, logs, Wi-Fi, Gateway ID, pairing, radio validation/restart, queue flush, Backend test/config, OTA, and confirmed reboot exercised |
| Backend upload | PASS | Queue drained through both fallback-AP local HTTP and STA local HTTP into real PostgreSQL |
| Authentication failure retention | PASS | Three HTTP 401 retries retained the queued record; restoring the token drained that same record |
| Queue reboot recovery | PASS | Two pending records recovered after software reboot, then drained exactly once |
| Browser-compatible OTA | PASS | Multipart upload booted inactive slot as `PENDING_VERIFY`, passed internal checks, and became `VALID` |
| Bootloader rollback | PASS | Controlled image booted `ota_0` pending, restarted before validation, and bootloader returned to valid `ota_1` |
| NTP synchronization | BLOCKED | Temporary host hotspot provided LAN service but its cross-zone internet forwarding remained unavailable; no trusted NTP timestamp was claimed |

Representative synthetic-path timing for sequence 3003 was:

- queue write: 17,769 us;
- RX-to-durable completion: 18,115 us;
- RX-to-ACK-start: 18,233 us;
- RX-to-ACK-complete: 382,915 us.

These are real Gateway execution, flash, and SX1278 transmit timings with a
synthetic post-RX packet. They are comfortably below the Node's 1,800 ms ACK
timeout, but they are not end-to-end RF timings.

## Gateway to PostgreSQL evidence

The local Backend used a randomly generated development token held only in a
mode-0600 temporary file. PostgreSQL stored the exact 42-byte raw packet and
decoded sequence 3001 as `rawDistanceMm=740`, `acceptedDistanceMm=739`, and
`temperatureCentiC=-1234`. Because NTP was untrusted, the Gateway correctly sent
`gatewayReceivedAt=null` and `gatewayTimeTrusted=false`; PostgreSQL added a
trusted `serverReceivedAt`. Monitoring history returned sequence 3001 as the
latest record at that point. A subsequent STA-delivered sequence 3003 also
created exactly one row and drained from the queue.

## Validation intentionally pending

| Observation | Result |
| --- | --- |
| Real Node telemetry reception | NOT TESTED — Node deployed in field |
| Node receives Gateway ACK | NOT TESTED — Node deployed in field |
| Node retry after lost ACK | NOT TESTED — Node deployed in field |
| End-to-end RF ACK latency | NOT TESTED — Node deployed in field |
| Real Node RSSI/SNR | NOT TESTED — Node deployed in field |

Run [pending-node-rf-validation.md](pending-node-rf-validation.md) when the
production Node is physically available.
