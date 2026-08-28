# LoRa Protocol 3 compatibility

Gateway 2.2.0 accepts only magic `GT` with `protocolVersion=3`. Protocol 3 is not backward compatible with Protocol 1 or 2; there is no legacy decoder, ACK, or dual-parser mode. Every multi-byte integer is explicitly encoded big-endian and packet lengths are exact.

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

The payload is exactly 57 bytes and the total packet is `62 + N` bytes. Its maximum size is 86 bytes for a 24-byte Node ID, below the existing 96-byte radio and durable-queue payload capacity.

| Relative to P | Size | Field | Encoding |
| ---: | ---: | --- | --- |
| 0 | 4 | persistentSessionId | uint32 |
| 4 | 4 | sequence | uint32 |
| 8 | 4 | medianEchoUs | uint32 |
| 12 | 4 | rawDistanceMm | uint32 |
| 16 | 4 | acceptedDistanceMm | uint32 |
| 20 | 2 | madMm | uint16 |
| 22 | 2 | temperatureCentiC | int16 two's complement |
| 24 | 2 | humidityCentiPercent | uint16 |
| 26 | 2 | batteryMv | uint16 |
| 28 | 1 | validSamples | uint8 |
| 29 | 1 | totalSamples | uint8 |
| 30 | 1 | filterState | enum |
| 31 | 2 | qualityFlags | bit mask |
| 33 | 2 | healthFlags | bit mask |
| 35 | 1 | bootReason | enum |
| 36 | 1 | rtcState | enum |
| 37 | 4 | rtcUnixTime | trusted UTC seconds or zero |
| 41 | 1 | pollIntervalMinutes | 1–255 |
| 42 | 1 | scheduleState | enum |
| 43 | 4 | scheduledMaintenanceUnix | UTC seconds or zero |
| 47 | 4 | lastCommandId | uint32; zero with NONE |
| 51 | 1 | lastCommandType | command enum |
| 52 | 1 | lastCommandResult | result enum; `FF` for NONE |
| 53 | 4 | referenceDistanceMm | uint32; absolute offset `58 + N` |

`referenceDistanceMm=0` means calibration is missing. Every value from 1 through `FFFFFFFF` is preserved exactly. Existing sentinels remain: distance `FFFFFFFF`, temperature `8000`, and humidity `FFFF`.

## ACK_COMMAND (`02`)

The fixed payload is 19 bytes followed by `L` command bytes, so total length is `24 + N + L`.

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

ACK identity repeats Node ID, persistent session, and telemetry sequence. An untrusted Gateway clock sends `gatewayUnixTime=0` and clears `timeValid`, but still ACKs.

| Command | Code | L | Payload |
| --- | ---: | ---: | --- |
| NONE | 0 | 0 | commandId must be zero |
| ENTER_MAINTENANCE_NOW | 1 | 0 | none |
| SCHEDULE_MAINTENANCE_AT | 2 | 4 | UTC Unix seconds |
| SET_POLL_INTERVAL_MINUTES | 3 | 1 | 1–255 minutes |

## COMMAND_RESULT (`03`)

The payload is exactly 15 bytes, so total length is `20 + N`.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | commandId |
| 8 | 1 | commandType |
| 9 | 1 | resultCode |
| 10 | 1 | effectivePollIntervalMinutes |
| 11 | 4 | effective scheduled-maintenance UTC |

Results are APPLIED=0, ALREADY_APPLIED=1, INVALID_ARGUMENT=2, RTC_UNAVAILABLE=3, RTC_TIME_UNTRUSTED=4, SCHEDULE_UNREPRESENTABLE=5, STORAGE_ERROR=6, and INTERNAL_ERROR=7.

## Queue/backend boundary

Only TELEMETRY is durably queued and uploaded. The exact Protocol 3 radio payload remains the Backend source of truth; Gateway does not reconstruct it or add a redundant trusted calibration field. COMMAND_RESULT stays Gateway-local. Backend remote commands, command tables, and scheduling APIs remain absent.

Protocol 3 uses SX1278 CRC but no HMAC, encryption, or node authentication.
