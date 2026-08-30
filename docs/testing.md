# Gateway testing

## Automated checks

```bash
pio test -e native
pio run -e esp32-c3-devkitm-1
node tools/test_dashboard_layout.mjs
```

Native tests cover Protocol 3 golden packets and validation, pairing and
session deduplication, durable-before-ACK ordering, queue recovery, Backend
Base64 serialization, command persistence and result matching, heartbeat
interval and schema validation, scheduler/queue priority, nullable metrics,
connectivity classification, and firmware/configuration values.

The production command compiles firmware for the ESP32-C3 target. The dashboard
layout check renders the embedded HTML at 1440, 800, and 360 pixels and rejects
page, card, form, input, and key/value overflow.

An additional live-browser form check is available when Chrome or Chromium can
reach a running Gateway dashboard:

```bash
node tools/test_dashboard_forms.mjs http://gathra-gateway.local/
```

Set `GATHRA_CHROME` when the browser executable is not in a supported default
location. This check exercises unsaved-form preservation against the running
dashboard and does not flash firmware.

## RF and durable-delivery validation

Hardware validation must use positively identified devices and verify:

1. valid Protocol 3 telemetry from the paired Node is committed before ACK;
2. a repeated Node/session/sequence tuple is re-ACKed without another queue
   record;
3. an unpaired or malformed packet is neither queued nor ACKed;
4. RX resumes after ACK transmission and after a bounded failure;
5. a recovered LittleFS record uploads with its capture-time boot identity;
6. `INSERTED`, `DUPLICATE`, and `REJECTED_INVALID` are handled per index;
7. transport/authentication/server failures retain queued records;
8. a matching command result confirms persistent command state without an RF
   ACK;
9. heartbeat activity never blocks radio capture and never enters the queue.

Build success is not evidence of RF reception, Node ACK reception, hardware
power behavior, OTA rollback, or field radio range. Record those results only
when directly observed.
