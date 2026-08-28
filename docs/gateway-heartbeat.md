# Gateway operational heartbeat contract

Gateway firmware 2.2.0 sends a compact best-effort operational snapshot to:

```http
POST /api/v1/iot/gateway/heartbeat
Content-Type: application/json
Authorization: Bearer <existing-gateway-token>
```

The HTTP contract has `schemaVersion: 1`. It is independent from LoRa Protocol
3; the LoRa version remains 3 and Protocol 3 packets are unchanged. Any HTTP
2xx response acknowledges a heartbeat. Response content is ignored.

## Scheduling and delivery

- Default interval: 60 seconds; local configurable range: 15–3600 seconds.
- The interval is persisted in Gateway NVS only after an explicit dashboard
  save. Schema-1 (firmware 2.1.0) config blobs migrate in place and retain
  identity, Wi-Fi, Backend token, radio, pairing, and upload settings.
- The low-priority Backend task attempts a heartbeat only when the durable
  telemetry queue is empty. It checks the queue again immediately before HTTP.
  Telemetry that arrives during the bounded request is still durably enqueued
  and ACKed by the independent priority-4 radio task.
- Heartbeat HTTP timeout is capped at five seconds even if the configured
  telemetry HTTP timeout is longer.
- Heartbeats never enter LittleFS or the durable telemetry queue. Failures are
  not replayed and do not use exponential backoff; the next attempt is at the
  next configured interval.
- 401, 404, other 4xx, 5xx, timeout, TLS/DNS failure, missing credentials, and
  disconnected Wi-Fi are bounded best-effort failures. They do not change or
  dequeue telemetry records.
- A heartbeat is serialized and attempted with `timeValid=false` before SNTP
  sync. TLS certificate/hostname validation is never disabled; a platform TLS
  failure caused by an unset clock remains a best-effort failure.
- The same existing bearer credential and validated TLS implementation are
  reused. Passwords and bearer tokens are never included in JSON or logs.

## Schema 1

All integer counters are JSON numbers. `At` fields are UTC RFC 3339 strings
with milliseconds or `null`. Byte fields are bytes; durations are seconds or
milliseconds as named; radio strength is dBm, SNR is dB, and frequency error is
Hz.

| Path | Type | Meaning |
|---|---|---|
| `schemaVersion` | integer | Always `1`. |
| `gateway.gatewayId` | string | Persisted logical Gateway ID. |
| `gateway.mac` | string | Authoritative Wi-Fi station MAC. |
| `gateway.firmwareVersion` | string | `2.2.0`. |
| `gateway.protocolVersion` | integer | LoRa Protocol `3`. |
| `gateway.buildFlavor` | string | Compile-time build flavor. |
| `runtime.uptimeSeconds` | integer | 64-bit monotonic `esp_timer` uptime. |
| `runtime.resetReason` | string | ESP reset-reason name. |
| `runtime.bootCount` | integer | Persistent saturating uint32 boot count. |
| `runtime.freeHeapBytes` | integer | Current free heap. |
| `runtime.minFreeHeapBytes` | integer | Minimum free heap observed by ESP-IDF since boot. |
| `runtime.largestFreeHeapBlockBytes` | integer | Largest current 8-bit-capable heap block. |
| `runtime.sketchSizeBytes` | integer | Running application image usage. |
| `runtime.freeSketchSpaceBytes` | integer | Arduino OTA-reported free application space. |
| `runtime.flashSizeBytes` | integer | Detected flash size. |
| `network.wifiConnected` | boolean | STA connection state. |
| `network.ssid` | string | Configured SSID; never a password. |
| `network.wifiRssiDbm` | number/null | Current STA RSSI, null while disconnected. |
| `network.localIp` | string/null | Current STA IPv4, null while disconnected. |
| `network.backendConnectivityState` | string | `UNKNOWN`, `HEALTHY`, `DEGRADED`, or `OFFLINE`. |
| `network.lastBackendSuccessAt` | string/null | Last successful durable telemetry batch with trusted time. |
| `network.lastBackendErrorAt` | string/null | Last failed durable telemetry HTTP/contract operation with trusted time. |
| `network.consecutiveBackendFailures` | integer | Consecutive durable telemetry operation failures. |
| `time.timeValid` | boolean | SNTP trust state. |
| `time.currentUtc` | string/null | Current trusted UTC, otherwise null. |
| `time.lastNtpSyncAt` | string/null | Latest SNTP callback time, otherwise null. |
| `time.ntpAgeSeconds` | integer/null | Current trusted UTC minus last sync, otherwise null. |
| `lora.pairedNodeId` | string/null | Paired Node ID, or null when unpaired. |
| `lora.lastLoRaRxAt` | string/null | Last successfully read SX1278 frame time when UTC was trusted. |
| `lora.latestRssiDbm` | number/null | Latest successfully read frame RSSI. |
| `lora.latestSnrDb` | number/null | Latest successfully read frame SNR. |
| `lora.latestFrequencyErrorHz` | integer/null | Latest successfully read frame frequency error. |
| `lora.receivedPacketCount` | integer | RX-done events handled since boot. |
| `lora.validTelemetryCount` | integer | Protocol 3 TELEMETRY packets decoded since boot, before pairing rejection. |
| `lora.invalidPacketCount` | integer | Sum of CRC, invalid-length, radio-read, and Protocol decode failures. |
| `lora.crcErrorCount` | integer | RadioLib CRC mismatch results since boot. |
| `lora.protocolRejectedPacketCount` | integer | Protocol decode rejections since boot. |
| `lora.unpairedRejectedPacketCount` | integer | Valid packets rejected for missing/wrong pairing since boot. |
| `ack.ackCount` | integer | ACK successes plus failures since boot. |
| `ack.ackSuccessCount` | integer | Radio TX completed and continuous RX was restored. It does not prove Node reception. |
| `ack.ackFailureCount` | integer | ACK encode/TX/timeout/RX-restore failures since boot. |
| `ack.latencySampleCount` | integer | Successful ACK samples represented by rolling statistics. |
| `ack.latestRxToAckStartMs` | number/null | Latest measured attempt, RX-done observation to TX request. |
| `ack.latestRxToAckCompleteMs` | number/null | Latest measured attempt, RX-done observation to TX completion/timeout boundary. |
| `ack.latestAckTxDurationMs` | number/null | Latest complete minus latest start. |
| `ack.minRxToAckStartMs` | number/null | Successful-ACK minimum since boot. |
| `ack.maxRxToAckStartMs` | number/null | Successful-ACK maximum since boot. |
| `ack.avgRxToAckStartMs` | number/null | Incremental successful-ACK mean since boot. |
| `ack.minRxToAckCompleteMs` | number/null | Successful-ACK minimum since boot. |
| `ack.maxRxToAckCompleteMs` | number/null | Successful-ACK maximum since boot. |
| `ack.avgRxToAckCompleteMs` | number/null | Incremental successful-ACK mean since boot. |
| `ack.minAckTxDurationMs` | number/null | Successful ACK TX-duration minimum since boot. |
| `ack.maxAckTxDurationMs` | number/null | Successful ACK TX-duration maximum since boot. |
| `ack.avgAckTxDurationMs` | number/null | Incremental successful ACK TX-duration mean since boot. |
| `queue.depth` | integer | Current durable telemetry records. |
| `queue.capacity` | integer | Derived bounded LittleFS queue capacity. |
| `queue.oldestRecordAgeSeconds` | integer/null | Age from one oldest-record peek, when derivable without a scan. |
| `queue.telemetryUploadSuccessCount` | integer | Successful terminal Backend batches since boot. |
| `queue.telemetryUploadFailureCount` | integer | Failed Backend HTTP/contract batch operations since boot. |
| `commands.pendingCommandId` | integer/null | Current PENDING/SENT command ID. |
| `commands.pendingCommandType` | string/null | Current PENDING/SENT Protocol 3 command name. |
| `commands.pendingCommandState` | string/null | `PENDING` or `SENT`. |
| `commands.lastCommandId` | integer/null | Persisted current/last command ID. |
| `commands.lastCommandResult` | string/null | Persisted current/last result name. |
| `commands.commandsSentCount` | integer | Successful ACK transmissions carrying a command since boot. |
| `commands.commandResultsReceivedCount` | integer | Valid COMMAND_RESULT packets received since boot. |

`backendConnectivityState` deliberately excludes heartbeat outcomes so a 404
while the Backend endpoint is being deployed cannot recursively make the
heartbeat describe telemetry delivery as offline. It is `UNKNOWN` before a
durable upload operation, `HEALTHY` after a success with no later failures,
`DEGRADED` after one or two consecutive failures, and `OFFLINE` after three.

## Gateway ACK timing boundaries

These are Gateway-observed latencies, not Node RTT:

1. RX boundary: the first `esp_timer_get_time()` in the radio worker after the
   SX1278 DIO0 RX-done notification. On-air packet reception is complete; packet
   reading, Protocol decoding, deduplication, and durable enqueue follow.
2. ACK start: `esp_timer_get_time()` immediately before RadioLib
   `startTransmit()`. This is the Gateway's asynchronous TX-request boundary.
3. ACK complete: `esp_timer_get_time()` after packet-sent notification and
   `finishTransmit()`, or after the bounded TX failure/timeout path, and before
   continuous RX restoration returns.

`rxToAckStartMs = start - RX`, `rxToAckCompleteMs = complete - RX`, and
`ackTxDurationMs = complete - start`. Rolling min/max/incremental averages use
successful transmissions only and retain no sample list.

## Persistence and reset scope

`heartbeatIntervalSeconds`, the existing Gateway config, current command, and
`bootCount` survive reboot. Boot count performs one NVS write per boot and
saturates at uint32 maximum. Uptime, heartbeat attempt counters, radio/ACK
counters, upload counters, and rolling statistics reset at boot. Existing
durable telemetry records and their independent lifetime queue diagnostics
remain persistent as before.
