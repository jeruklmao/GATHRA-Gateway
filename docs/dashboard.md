# Local operations dashboard

The self-contained dashboard is served on every active Gateway interface. It
uses embedded HTML, CSS, and JavaScript only—no CDN or external asset.

## Sections

- **Gateway Status:** firmware/build/Git metadata, immutable MAC, editable
  logical ID, boot identity, uptime, reset reason, heap, flash, firmware/OTA
  space, filesystem use, and radio/Wi-Fi/Backend/time/pairing state.
- **Wi-Fi:** configured SSID, connection, IP, RSSI, reconnect and fallback AP;
  SSID/password replace provisioning.
- **Node Pairing:** paired state, discovery state and candidate metadata,
  manual pairing, cancel, confirm, and confirmed unpair.
- **Latest Node Telemetry:** paired production Node identity, echo/distance/MAD,
  nullable temperature/humidity/distance, battery, samples, filter state, and
  exact quality/health flag integers.
- **LoRa Diagnostics:** applied parameters, RadioLib state/code, RX/CRC/decode/
  unknown/duplicate/ACK counters, RF metadata, queue-write duration, and the
  three required RX-to-durable/ACK-path latencies.
- **Durable Queue:** depth/capacity/utilization, oldest known age, upload,
  retry, dedup, rejection, overflow and filesystem counters, plus worker wake.
- **Backend:** URL, credential-presence boolean, transport policy, attempt,
  status, success, error and backoff; token replace/clear and compatibility
  test.
- **NTP / Time:** explicit `SYNCED`/`UNSYNCED`, current trusted UTC and last
  observed synchronization.
- **Logs:** bounded RAM-only recent log ring.
- **OTA:** inactive-slot browser `.bin` upload and reboot.
- **Controls:** confirmed Gateway reboot.

Status APIs never return the Wi-Fi password or Bearer token. Logs do not print
either value. The dashboard has no application-level authentication in v1;
this is a known limitation, not an accidental omission.

The five-second status refresh continues while an operator edits settings, but
it never repopulates a form after that form receives unsaved input. This keeps
an edited SSID from reverting when focus moves to the password field and also
protects multi-field radio and Backend edits. A failed save leaves the entered
values intact. A successful save clears the form's dirty state and removes
password values from the browser DOM.

The WebServer executes below the radio worker. A slow client can delay another
dashboard request but cannot make the radio worker wait for HTTP, DNS, or HTML
rendering. OTA flash activity is the explicit exception and is shown as
capture-unavailable.
