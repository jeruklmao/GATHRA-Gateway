# GATHRA LoRa Protocol 3

Gateway firmware 2.2.0 accepts Protocol 3 packets with magic `GT` and exact
length validation. Multi-byte integers are encoded big-endian.

## Common header

Let `N` be `nodeIdLength` and `P = 5 + N`.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | ASCII `GT` (`47 54`) |
| 2 | 1 | protocolVersion | `03` |
| 3 | 1 | messageType | `01` TELEMETRY, `02` ACK_COMMAND, `03` COMMAND_RESULT |
| 4 | 1 | nodeIdLength | 1–24 |
| 5 | N | nodeId | ASCII letters, digits, hyphen, underscore |

## TELEMETRY (`01`)

The payload is 57 bytes and total packet length is `62 + N`; the maximum is 86
bytes for a 24-byte Node ID.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | sequence |
| 8 | 4 | medianEchoUs |
| 12 | 4 | rawDistanceMm |
| 16 | 4 | acceptedDistanceMm |
| 20 | 2 | madMm |
| 22 | 2 | temperatureCentiC, signed two's complement |
| 24 | 2 | humidityCentiPercent |
| 26 | 2 | batteryMv |
| 28 | 1 | validSamples |
| 29 | 1 | totalSamples |
| 30 | 1 | filterState |
| 31 | 2 | qualityFlags |
| 33 | 2 | healthFlags |
| 35 | 1 | bootReason |
| 36 | 1 | rtcState |
| 37 | 4 | rtcUnixTime |
| 41 | 1 | pollIntervalMinutes |
| 42 | 1 | scheduleState |
| 43 | 4 | scheduledMaintenanceUnix |
| 47 | 4 | lastCommandId |
| 51 | 1 | lastCommandType |
| 52 | 1 | lastCommandResult |
| 53 | 4 | referenceDistanceMm |

Unavailable sentinels are distance `FFFFFFFF`, temperature `8000`, and humidity
`FFFF`. `referenceDistanceMm=0` means no Node calibration reference.

## ACK_COMMAND (`02`)

The fixed payload is 19 bytes followed by `L` command bytes, for total length
`24 + N + L`.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | telemetry sequence |
| 8 | 4 | gatewayUnixTime |
| 12 | 1 | flags; bit 0 is `timeValid` |
| 13 | 4 | commandId |
| 17 | 1 | commandType |
| 18 | 1 | commandPayloadLength |
| 19 | L | command payload |

ACK identity must repeat the Node ID, persistent session, and sequence. An
untrusted clock sends `gatewayUnixTime=0` with `timeValid=0`.

| Command | Code | L | Payload |
| --- | ---: | ---: | --- |
| NONE | 0 | 0 | commandId is zero |
| ENTER_MAINTENANCE_NOW | 1 | 0 | none |
| SCHEDULE_MAINTENANCE_AT | 2 | 4 | UTC Unix seconds |
| SET_POLL_INTERVAL_MINUTES | 3 | 1 | 1–255 minutes |

## COMMAND_RESULT (`03`)

The payload is 15 bytes and total length is `20 + N`.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | commandId |
| 8 | 1 | commandType |
| 9 | 1 | resultCode |
| 10 | 1 | effectivePollIntervalMinutes |
| 11 | 4 | effective scheduled-maintenance UTC |

Result codes are `APPLIED`, `ALREADY_APPLIED`, `INVALID_ARGUMENT`,
`RTC_UNAVAILABLE`, `RTC_TIME_UNTRUSTED`, `SCHEDULE_UNREPRESENTABLE`,
`STORAGE_ERROR`, and `INTERNAL_ERROR` with numeric values 0–7.

Only TELEMETRY is queued and uploaded. The Backend independently decodes the
exact radio payload. COMMAND_RESULT updates local persistent command state and
receives no radio ACK.

Protocol 3 uses SX1278 CRC but no HMAC, encryption, or Node authentication.
