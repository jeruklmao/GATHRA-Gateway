# Browser OTA, validation, and rollback

## Partition model

`partitions.csv` contains OTA metadata and two equal 1,408 KiB application
slots. Browser upload uses Arduino `Update` with `U_FLASH`, which writes the
inactive slot, verifies completion, selects it, and then schedules reboot.
LittleFS occupies its separate 1,152 KiB partition and is not an OTA target.

```bash
pio run -e esp32-c3-devkitm-1
curl --fail -F \
  'firmware=@.pio/build/esp32-c3-devkitm-1/firmware.bin' \
  http://192.168.4.1/api/ota
```

The same `/api/ota` handler serves the browser and command-line workflow. It
expects the PlatformIO application artifact `firmware.bin`, not a merged
factory image, bootloader image, or partition-table image.
During upload the dashboard and logs state that telemetry capture is not
guaranteed.

## Meaningful boot validation

Arduino-ESP32's early automatic confirmation is deferred with
`verifyRollbackLater()`. A `PENDING_VERIFY` image is marked valid only after:

- versioned configuration/NVS initializes and validates;
- LittleFS mounts and durable queue recovery completes;
- the expected OTA data, equal application slots, and LittleFS partition are
  present and within 4 MiB;
- core initialization reaches the validation point.

Wi-Fi, NTP, Backend, Node traffic, and successful radio hardware initialization
are deliberately not required. External availability must not roll back sound
firmware.

## Controlled rollback profile

`rollback-test` defines `GATHRA_ROLLBACK_TEST_FAIL=1`. It restarts before
validation only if it is actually booted as `PENDING_VERIFY`. On the next boot,
the bootloader should mark it aborted and select the prior valid slot.

```bash
pio run -e rollback-test
curl --fail -F \
  'firmware=@.pio/build/rollback-test/firmware.bin' \
  http://192.168.4.1/api/ota
```

Do not claim rollback PASS until serial logs and partition state show an actual
return to the earlier image. Always restore the production profile after this
test. Never ship the rollback-test image as the final firmware.
