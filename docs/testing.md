# Gateway testing

## Automated

~~~bash
pio test -e native
pio run -e esp32-c3-devkitm-1
~~~

Native tests cover Protocol 2 TELEMETRY/ACK_COMMAND/COMMAND_RESULT golden bytes, big-endian fields, wrong versions and malformed lengths, NONE and all required commands, timeValid true/false, every result code, pairing/session deduplication, durable-before-ACK behavior, command allocator persistence, pending/restart/resend, exact result matching, wrong ID ignore, duplicate result, confirmed-not-resent, queue recovery, and existing configuration/queue logic.

## USB/RF HIL evidence (2026-08-25)

Gateway USB identity was positively mapped to MAC 10:00:3B:D4:E9:58. Firmware 2.0.0 initialized SX1278, recovered LittleFS and NVS command state, joined configured Wi-Fi at 192.168.100.34, and synchronized NTP.

A Node v2 pairing candidate was observed at RSSI -45 dBm, SNR 10.5 dB and frequency error about +1274 Hz. After pairing, real packets were durably enqueued before ACK. Representative timing was queue write 14.273 ms, RX-to-durable 15.900 ms, RX-to-ACK-start 16.089 ms, and RX-to-ACK-complete 626.828 ms; later flash writes produced bounded ACK starts up to 74.735 ms. Node ACK reception was first-attempt success.

SET_POLL_INTERVAL_MINUTES=5 returned APPLIED and CONFIRMED. A SCHEDULE_MAINTENANCE_AT command remained PENDING through a Gateway software reboot, was restored with nextCommandId=3, resent, and confirmed. ENTER_MAINTENANCE_NOW also confirmed. The Node reported Gateway UTC and corrected an INVALID_VL RTC.

For an intentional result-loss test, command 5 set the interval to 2 minutes.
The Gateway was moved to 450 MHz only during COMMAND_RESULT reception, so its
state remained SENT after Node persistence/application. On the next telemetry
the same ID was resent (`sendCount=2`); the Node did not repeat the side effect,
re-sent its stored APPLIED result, and Gateway changed to CONFIRMED. Command 10
later restored the production interval to 10 minutes.

Battery-only RF testing observed repeated `RTC_TIMER` hard-power boots, manual
and command maintenance, two normal TF polls while an alarm remained pending,
the exact minute-aligned AF wake, one-shot completion, and two successful Node
OTA latch-preserving reboots. Representative battery-run RF was RSSI -39 to
-42 dBm, SNR 13.2 to 14.25 dB, frequency error about +1.2 kHz, with ACK start
19.383 to 76.279 ms and ACK completion 629.949 to 687.058 ms after RX.

Browser OTA uploaded 1233024 bytes, booted ota_1 PENDING_VERIFY, restored pairing/command state, and marked the image valid after checks.

The currently deployed Backend still returned a v1-only permanent rejection during this bench run. The repository's minimal v2 decoder/migration tests pass, but deployment is a separate operational action.

## Network safety

The test laptop mapping was RTL8188 wlp0s20f0u1 for Internet and Intel AX211 wlp0s20f3 for local GATHRA AP access. Only AX211 was connected to the Node AP; the RTL connection and default Internet route were not modified. Remove the temporary AX211 NetworkManager profile after battery HIL.
