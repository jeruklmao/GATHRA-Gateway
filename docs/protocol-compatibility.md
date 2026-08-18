# Node Protocol v1 compatibility

The authoritative reference is the current `GATHRA-Node` implementation,
especially `lib/protocol/protocol.cpp`; documentation is secondary. Gateway
tests contain the exact golden telemetry bytes produced by the Node codec and
verify the exact ACK bytes.

## Framing

- Magic: `47 54` (`GT`)
- Version: `01`
- Telemetry type: `01`
- ACK type: `02`
- Integer byte order: big-endian/network order
- Node ID: `[A-Za-z0-9_-]{1,24}`
- Telemetry length: `40 + N`
- ACK length: `13 + N`
- SX1278 packet CRC: enabled

After the telemetry prefix, the Gateway decodes Node boot/session ID, sequence,
median echo, raw and accepted distance, MAD, temperature, humidity, battery,
sample counts, filter-state code, quality flags, and health flags at the exact
Node offsets. Unknown quality/health bits are preserved.

Unavailable sentinels are retained in the raw payload and rendered locally as
unavailable:

| Field | Sentinel |
| --- | ---: |
| raw distance | `UINT32_MAX` |
| accepted distance | `UINT32_MAX` |
| temperature | `INT16_MIN` |
| humidity | `UINT16_MAX` |

The Backend independently repeats the binary validation and normalizes those
sentinels to SQL `NULL`.

## ACK identity

ACK includes the same Node ID followed by the exact Node boot/session ID and
sequence. A retry of a known tuple is ACKed again without a second enqueue.
Protocol v1 intentionally has no LoRa authentication or HMAC; pairing is an
operational filter, not cryptographic identity.
