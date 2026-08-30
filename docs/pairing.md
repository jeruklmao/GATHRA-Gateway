# Single-Node pairing

Gateway 2.2.0 stores exactly one `pairedNodeId` in NVS. It has no Node table,
scheduler, mesh, LoRaWAN, or multi-Node fairness logic.

## Discovery pairing

1. In `UNPAIRED`, select **Start Pairing**.
2. Valid Protocol 3 telemetry is displayed as a candidate with Node ID,
   boot/session ID, sequence, RSSI, and SNR.
3. Candidate traffic is observation-only: it is not added to the telemetry
   queue and is not ACKed.
4. Select **Confirm Pairing**. NVS persistence must succeed.
5. Production capture begins with the next matching packet.

The Gateway never silently pairs to the first transmitter. Pairing discovery
must be cancelled or explicitly confirmed.

## Manual pairing and unpair

Manual entry accepts only `[A-Za-z0-9_-]{1,24}`. **Unpair** requires an
explicit `UNPAIR` confirmation from the UI. Once paired, only the exact Node ID
is queued and ACKed; other valid packets increment unknown-Node diagnostics
and are ignored.

Pairing is not radio authentication. A transmitter that knows the paired ID can
forge Protocol 3 packets. Pairing is therefore not an authentication boundary.
