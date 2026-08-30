# GATHRA Gateway

GATHRA Gateway firmware **2.2.0** bridges one paired GATHRA Node from **LoRa
Protocol 3** to the GATHRA Backend. It runs on an ESP32-C3 Super Mini with an
SX1278, persistent configuration and command state in NVS, and a durable
LittleFS telemetry queue.

## Data path

```text
GATHRA Node
  -> LoRa Protocol 3 TELEMETRY
  -> validate pairing and deduplicate
  -> atomically enqueue exact packet in LittleFS
  -> ACK_COMMAND with current UTC trust and at most one command
  -> restore continuous LoRa receive
  -> asynchronous authenticated HTTPS batch to GATHRA Backend
```

The radio worker owns the latency-sensitive receive, durable-before-ACK, ACK,
and RX-restoration sequence. NTP and Backend operations run independently and
are never awaited by the ACK path. If Gateway time is untrusted, the ACK clears
`timeValid`, sends UTC zero, and still acknowledges valid durable telemetry.

Only the exact paired Node ID is queued and acknowledged. Pairing discovery
shows candidates but does not queue or ACK them. Pairing is an operational
allow-list, not cryptographic identity.

## Backend forwarding

Telemetry batches use schema 1 and contain the exact Base64 Protocol 3 packet,
Gateway identity, capture-time boot identity, trusted nullable reception time,
uptime, RSSI, SNR, frequency error, and packet length. The default batch size is
20 and the maximum is 50. Records leave LittleFS only after HTTP 200 and a
validated terminal result for every submitted index.

The queue survives restart, uses checksummed atomic record files, and suppresses
duplicate Node/session/sequence tuples. Storage failure withholds the ACK.

## Commands

The local dashboard can queue one persistent command:

- `ENTER_MAINTENANCE_NOW`
- `SET_POLL_INTERVAL_MINUTES` for 1–255 minutes
- `SCHEDULE_MAINTENANCE_AT` for a trusted, minute-aligned UTC target

The command is repeated in later telemetry ACKs until the matching Protocol 3
`COMMAND_RESULT` confirms it. Command results remain Gateway-local and receive
no radio ACK. The Backend has no remote command API.

## Operational heartbeat

The Gateway sends a best-effort schema-1 heartbeat to
`POST /api/v1/iot/gateway/heartbeat` with the same authenticated Backend
transport. The interval defaults to 60 seconds and is configurable and
persistent from 15–3600 seconds. Heartbeats:

- run only while the durable telemetry queue is empty;
- use a maximum five-second HTTP timeout;
- never enter LittleFS or delay the independent radio worker;
- are not replayed after failure;
- contain operational metrics but no Wi-Fi password or bearer credential.

See [gateway-heartbeat.md](docs/gateway-heartbeat.md) for the exact fields and
freshness contract.

## Local dashboard and OTA

Wi-Fi STA configuration, fallback AP, Gateway identity, pairing, radio,
Backend delivery, heartbeat interval, queue, commands, logs, and OTA are managed
through the local dashboard. Stored Wi-Fi and Backend credentials are never
returned by status endpoints.

Browser OTA writes the inactive application slot. A pending image is accepted
only after configuration, NVS, LittleFS queue recovery, and partition-layout
checks succeed. See [provisioning](docs/provisioning.md),
[dashboard](docs/dashboard.md), and [OTA](docs/ota.md).

## Build and test

```bash
pio test -e native
pio run -e esp32-c3-devkitm-1
node tools/test_dashboard_layout.mjs
```

The native suite validates Protocol 3, durable-before-ACK behavior, queue and
Backend contracts, commands, heartbeat scheduling/schema, and configuration.
The layout check verifies the embedded dashboard at desktop, tablet, and mobile
widths. See [testing.md](docs/testing.md).

## Security and safety

Production Backend delivery uses HTTPS certificate and hostname validation plus
a provisioned bearer credential. Protocol 3 uses SX1278 CRC but does not provide
encryption or Node authentication. Keep the dashboard limited to a trusted
local network and provision unique Wi-Fi and Backend credentials.

Gateway telemetry and operational status are observations, not proof that a
monitored area or route is safe.

---

Copyright © 2026 GATHRA Project. All rights reserved.

Source code and documentation in this repository are publicly viewable for inspection, academic review, and evaluation. No permission is granted to reproduce, redistribute, modify, commercialize, or create derivative works except where explicitly permitted by the repository's license or by written permission from the copyright holder.

If you use GATHRA in academic or research work, please provide appropriate attribution to the GATHRA Project and its associated publications.
