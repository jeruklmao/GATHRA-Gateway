# Gateway architecture

Firmware 2.2.0 separates the latency-critical LoRa path from network work.

## Radio path

A priority-4 receive worker owns SX1278 receive/transmit state. For TELEMETRY it performs exact Protocol 3 decoding, including the appended calibration reference, pairing validation, persistent-session sequence deduplication, and atomic LittleFS enqueue before calling the ACK transmitter. ACK_COMMAND is constructed with a current trusted-time snapshot and a copy of the persisted pending command. RX is restored immediately after transmission.

The Backend worker and NTP are never awaited. If time is untrusted, flags.timeValid=0 and gatewayUnixTime=0 while ACK reliability continues.

COMMAND_RESULT is validated separately. It must match the paired Node, latest persistent session, current command ID and command type. A matching result atomically changes command state to CONFIRMED; wrong and stale results are ignored. It is not acknowledged.

## Command state

CommandStore contains a persistent uint32 nextCommandId and one bounded command record. Creation increments and saves the allocator together with PENDING state, preventing ID reuse after reboot. SENT records remain eligible for every later ACK. A Gateway reboot restores PENDING/SENT and retransmits; CONFIRMED is not retransmitted. Access from the radio and dashboard tasks is serialized with a mutex.

States are NONE, PENDING, SENT, CONFIRMED, FAILED and CANCELLED. Only matching COMMAND_RESULT makes CONFIRMED.

## Durable telemetry

Queue files are checksummed, atomically renamed records containing exact raw RF bytes and reception metadata. Recovery validates record framing and Protocol 3 telemetry before indexing. The 86-byte maximum Protocol 3 telemetry packet fits the 96-byte payload slot. Queue capacity is bounded; a storage failure withholds ACK so receipt is never falsely claimed. Backend upload is asynchronous HTTPS.

The live telemetry view and current known poll interval only use the explicitly paired production Node.

## Operational heartbeat

The existing low-priority Backend worker owns heartbeat scheduling and HTTP.
It drains durable telemetry first and only starts a heartbeat while queue depth
is zero. The worker rechecks before HTTP, caps heartbeat requests at five
seconds, and never writes heartbeat data to LittleFS. A packet arriving during
HTTP is still handled, persisted, and ACKed by the independent priority-4 radio
task; its upload follows when the bounded heartbeat call returns.

The heartbeat is a current in-memory snapshot. Failures are not accumulated or
replayed and are isolated from durable-upload connectivity state. Rolling ACK
statistics use constant memory and an incremental mean. See
`gateway-heartbeat.md` for the schema and exact timer boundaries.

## Time

SNTP maintains UTC trust. Every ACK asks TimeService for a new snapshot close to packet generation. The dashboard reports UNSYNCHRONIZED/SYNCED state and exact last/current sync. Scheduled commands require trusted NTP and a minute-aligned UTC target 60 seconds to 27 days ahead.

## OTA and watchdog boundaries

OTA writes the inactive application slot, boots PENDING_VERIFY, and marks valid only after internal configuration/filesystem/radio/Wi-Fi service checks. The dashboard warns that telemetry capture is unavailable during OTA/reboot.
