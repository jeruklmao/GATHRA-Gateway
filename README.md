# GATHRA Gateway

GATHRA Gateway firmware 2.1.0 is the single-paired-Node bridge for LoRa Protocol 3. It decodes v3 TELEMETRY, durably spools the exact packet before ACK, sends ACK_COMMAND immediately with fresh UTC trust state and at most one persisted command, restores RX, and uploads telemetry asynchronously. Backend or NTP delay never blocks the ACK path.

Protocols 1 and 2 are intentionally unsupported.

## Reliability pipeline

~~~text
SX1278 DIO0
  -> validate v3 and paired Node
  -> deduplicate by nodeId + persistentSessionId + sequence
  -> atomic LittleFS enqueue
  -> build ACK_COMMAND with current timeValid/UTC and pending command
  -> transmit ACK and restore continuous RX
  -> asynchronous HTTPS Backend batch
~~~

COMMAND_RESULT does not enter the telemetry queue and receives no RF ACK. A command remains PENDING/SENT and is repeated in future ACKs until the exact paired node/session, command ID, and type return a result.

## Local Node control

The dashboard provides confirmed Enter Maintenance Now, 1–255 minute poll interval, and future scheduled maintenance controls. Scheduling is disabled unless Gateway NTP is trusted; browser local time is displayed alongside UTC. Commands are local to the Gateway. No Backend command API or remote scheduling is implemented.

Command allocator and the single pending/confirmed record are persisted in NVS, including created time, last sent time, count, state, result and effective values.

## Build and test

~~~bash
pio test -e native
pio run -e esp32-c3-devkitm-1
~~~

Firmware uses ESP32-C3 Super Mini hardware, SX1278 at 433 MHz, LittleFS durable queue, NVS configuration/command state, Wi-Fi STA with fallback AP, browser OTA with rollback, trusted NTP, and HTTPS Backend delivery.

See docs/protocol-compatibility.md, docs/architecture.md, docs/dashboard.md, and docs/testing.md.

Protocol 3 still has radio CRC but no HMAC/authentication. Pairing is an operational allow-list, not cryptographic identity.
