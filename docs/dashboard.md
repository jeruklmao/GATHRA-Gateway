# Gateway dashboard

The dashboard shows firmware 2.1.0, radio/queue/backend state, paired Node identity, latest Protocol 3 diagnostics, RSSI/SNR/frequency error, RX-to-durable and RX-to-ACK timings, UTC/NTP trust, command state, Wi-Fi, OTA and bounded logs. Latest Node Telemetry renders a configured `referenceDistanceMm` with millimetre units; wire value zero is shown as unavailable with `calibration=missing`.

## Node Control

Only one command can be pending.

- Enter Maintenance Now requires an explicit confirmation dialog.
- Set Poll Interval accepts 1–255 minutes.
- Scheduled Maintenance accepts browser-local date/time, renders the corresponding UTC ISO value, and sends minute-aligned UTC seconds. The button is disabled when Gateway time is untrusted.
- Cancel requires the exact pending command ID and confirmation.

Status includes commandId, type, payload, created time, last sent time, sendCount/retry count, NONE/PENDING/SENT/CONFIRMED/FAILED/CANCELLED state, result, effective interval/target, and the latest known Node poll interval. A successful HTTP queue action means only that the command is durably pending; CONFIRMED requires matching RF COMMAND_RESULT.

## Pairing and operations

Pairing discovery displays valid v3 telemetry candidates but does not enqueue or ACK them. Confirmation persists one Node ID. Manual pairing remains available. Pairing is not cryptographic authentication.

Wi-Fi, Gateway ID, radio, Backend, queue flush, logs and OTA controls retain existing behavior. Node command control is entirely local-dashboard; no Backend request is involved.
