# Testing and HIL

## Automated host tests

```bash
pio test -e native
```

The suite uses the exact golden telemetry and ACK vectors from
`GATHRA-Node/test/test_main.cpp`. It covers Protocol v1 framing and endian
values, Node ID bounds, sentinel interpretation, ACK matching, explicit
pairing and paired-node enforcement, exact-tuple dedup, durable queue codec,
recovery/corruption/overflow, durable-before-ACK order, duplicate re-ACK,
configuration validation, batch JSON structure, per-reading boot identity,
terminal statuses, partial responses, and HTTP retry policy.

Production and diagnostic profiles:

```bash
pio run -e esp32-c3-devkitm-1
pio run -e hil
pio run -e rollback-test
```

## USB and serial

Rediscover the board every time; do not assume a stable device path.

```bash
pio device list
pio run -e hil --target upload --upload-port /dev/ttyACM0
timeout 30s pio device monitor --port /dev/ttyACM0 --baud 115200
```

Expected boot evidence includes version/build/Git metadata, 4 MiB flash,
LittleFS recovery and derived record capacity, SX1278 initialization with the
documented defaults, continuous `RECEIVING`, fallback AP, WebServer startup,
and OTA partition validation.

## Synthetic post-RX path

Only the `hil` image accepts these serial commands:

```text
HIL_PAIR
HIL_INJECT 1001
HIL_INJECT 1001
HIL_SUPPRESS_ACK_ONCE
HIL_INJECT 1002
HIL_RADIO_CYCLE
HIL_UNPAIR
```

`HIL_INJECT` enters at the point immediately after successful physical packet
read and uses the canonical Node packet. It exercises production decode,
pairing, dedup, LittleFS append, ACK encoding/real SX1278 transmit, immediate RX
restart, queue serialization, and—when configured—Backend delivery. The repeat
of sequence 1001 must re-ACK without increasing queue depth. Suppression is a
diagnostic only; it never exists in production.

`HIL_RADIO_CYCLE` exercises SX1278 standby, sleep, reinitialization with the
persisted settings, and continuous RX restart.

## Wi-Fi adapter isolation

On the development host used for this project, identify interfaces by USB ID
and driver before connecting. The Realtek RTL8188EUS (`0bda:8179`, `rtl8xxxu`)
is the permitted Gateway-AP client. Do not disconnect, reconfigure, or route
through the Intel `iwlwifi` adapter.

Example after resolving the Realtek interface name:

```bash
nmcli device wifi list ifname wlp0s20f0u1
nmcli device wifi connect GATHRA-GW-XXXXXX \
  password sman35jakarta ifname wlp0s20f0u1
curl --interface wlp0s20f0u1 http://192.168.4.1/api/status
```

An isolated Backend can be published on the host's Realtek AP-side address
(normally `192.168.4.2`) and configured as `http://192.168.4.2:3000`. This
development-only topology can drain the queue without moving the Intel
adapter or provisioning an external WLAN.

## Truthful result policy

Record only observed evidence. With the production Node deployed elsewhere,
real Node RX, Node-side ACK success/retry, RF latency, and real RSSI/SNR remain
`NOT TESTED — Node deployed in field`; follow
[pending-node-rf-validation.md](pending-node-rf-validation.md) later.
