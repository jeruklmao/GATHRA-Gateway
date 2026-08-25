# LoRa Protocol 2 compatibility

Gateway 2.0.0 accepts only magic GT, protocolVersion 2. There is no v1 decoder, legacy ACK, or dual-parser mode.

Message types are TELEMETRY=0x01, ACK_COMMAND=0x02, COMMAND_RESULT=0x03. Every multi-byte field is big-endian and all packet lengths are exact. The canonical byte layout, sentinels, enum values, flags, and alarm scheduling constraints are documented in the matching Node docs/protocol.md.

## ACK behavior

ACK identity repeats nodeId, persistentSessionId, and telemetry sequence. It contains gatewayUnixTime plus bit0 timeValid on every response. An untrusted clock sends timestamp zero and still ACKs.

Command codes are NONE=0, ENTER_MAINTENANCE_NOW=1, SCHEDULE_MAINTENANCE_AT=2, SET_POLL_INTERVAL_MINUTES=3. NONE requires commandId=0. Payload lengths are 0, 4-byte UTC, and 1-byte minutes respectively.

Results are APPLIED=0, ALREADY_APPLIED=1, INVALID_ARGUMENT=2, RTC_UNAVAILABLE=3, RTC_TIME_UNTRUSTED=4, SCHEDULE_UNREPRESENTABLE=5, STORAGE_ERROR=6, INTERNAL_ERROR=7.

## Queue/backend boundary

Only TELEMETRY is durably queued and uploaded. The exact v2 radio payload remains the Backend source of truth. COMMAND_RESULT stays Gateway-local in this release. Backend remote commands, command tables and scheduling APIs are intentionally absent.

Protocol 2 uses SX1278 CRC but no HMAC/authentication.
