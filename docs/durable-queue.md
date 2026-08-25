# Durable store-and-forward queue

## Record and commit format

LittleFS is used as a bounded spool; NVS is not used as a telemetry database.
Each measurement is one monotonically named `.rec` file. Its versioned binary
frame contains:

- magic, schema version, encoded length, and local record ID;
- trusted UTC receive milliseconds or the explicit untrusted `-1` marker;
- receive uptime and the Gateway boot/session identity from capture time;
- RSSI, SNR, frequency error, and packet length;
- exact raw LoRa payload bytes;
- CRC-32 over the complete frame except the CRC itself.

Append writes a `.tmp`, checks the full byte count, flushes and closes it, then
renames it to `.rec`. Startup deletes incomplete temporary files, validates
framing/CRC/Protocol v2 for each committed file, removes corrupt records with
diagnostics, sorts by record ID, and resumes at the next ID.

The runtime index is deliberately compact: each queued record retains only its
64-bit record ID and a 64-bit tuple hash in RAM. A hash match is always verified
against the checksummed on-flash packet before it is considered an exact
duplicate, so hash collisions cannot cause telemetry loss. This keeps the
filesystem-derived 3,182-record capacity practical on ESP32-C3 RAM.

## Durability and ACK

The radio path sees success only after the committed rename. ACK is withheld
on any failure. No compaction or full scan occurs in this path. Backend upload
only peeks records and deletes a record after a terminal per-record result:

- `INSERTED` -> delete and remember tuple;
- `DUPLICATE` -> delete and remember tuple;
- `REJECTED_INVALID` -> delete, remember tuple, increment permanent rejection,
  and log the reason;
- transport error, timeout, non-200, authentication failure, malformed or
  missing result -> retain for retry.

A checksummed 64-entry recent-tuple ring is stored alongside the queue so a
lost Node ACK can still be re-ACKed after uploaded records are removed and the
Gateway restarts. PostgreSQL uniqueness remains the final cross-reboot safety
net.

## Capacity and overflow

The 4 MiB partition table assigns `0x120000` = 1,179,648 bytes (1,152 KiB) to
LittleFS. A maximum record is 152 bytes (56 bytes framing plus the configured
96-byte radio capacity). Firmware reserves 64 KiB, budgets 128 bytes per-file
filesystem overhead, retains another 20% copy-on-write margin, and caps at
4,096:

```text
floor((1,179,648 - 65,536) / (152 + 128)) * 4 / 5 = 3,182 records
```

The mounted filesystem's reported total, shown on the dashboard, is the
runtime authority and may make the derived capacity lower than this partition
calculation.

When logical capacity or physical storage is full, v1 may remove exactly the
oldest record and retry the newest append. The dropped-oldest counter is
incremented and checkpointed, and an ERROR log is emitted after RX is restored.
The newest packet is ACKed only if its own durable write succeeds. If safe
replacement fails, it is not ACKed. Data loss is never silent.

Raw history has no automatic Backend retention policy in v1.
