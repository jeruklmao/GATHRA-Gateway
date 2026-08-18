# Pending real-Node RF validation

Run this procedure later with the production Node physically available. Do not
change the immutable GPIO map or radio defaults to force a result.

## Preparation

1. Flash the latest production Gateway image and capture serial at 115200 baud.
2. Confirm `SX1278 ready ... CRC=on` and radio state `RECEIVING`.
3. Open the dashboard through the dedicated Realtek test adapter or an isolated
   LAN and record initial queue/counter values.
4. Unpair, start pairing discovery, then power/wake the Node once.

## Pair and first capture

1. Verify candidate Node ID, boot/session ID, sequence, RSSI, and SNR match the
   Node serial/dashboard observation.
2. Confirm pairing. Verify the discovery packet was not queued or ACKed.
3. Trigger the next Node measurement.
4. In Gateway logs verify strict order: RX, durable persistence, ACK transmit,
   RX restored. Queue depth must increase before ACK success is logged.
5. Record dashboard microsecond values for RX-to-enqueue completion,
   RX-to-ACK-start, and RX-to-ACK-complete. Compare ACK complete against the
   Node's 1,800 ms timeout.
6. Confirm the Node reports the exact ACK tuple accepted.
7. Record real RSSI, SNR, frequency error, and packet length from Gateway.

## Duplicate and retry behavior

1. Use a controlled diagnostic build or test hook to suppress exactly one ACK;
   do not disable durable enqueue.
2. Verify the first attempt was durably stored but not ACKed.
3. Verify the Node retries the same Node ID, boot/session ID, and sequence.
4. Verify Gateway classifies it duplicate, does not increase queue depth, and
   transmits the ACK again.
5. Verify the Node accepts the retry ACK and does not exceed its configured
   maximum three TX attempts.

## Backend correlation

1. Ensure the Gateway is configured with a reachable test Backend and token.
2. Query PostgreSQL/monitoring API for the same Node ID, boot/session ID, and
   sequence.
3. Verify exactly one `iot_telemetry` row, byte-identical raw payload, matching
   reception metadata, and the measured Gateway capture-time boot identity.
4. If the HTTP response is intentionally lost, verify a retry returns
   `DUPLICATE` and PostgreSQL still contains one row.

Preserve bounded serial logs, API responses, and database queries as evidence.
Mark any unavailable observation `NOT TESTED`; never infer Node ACK success from
Gateway TX success alone.
