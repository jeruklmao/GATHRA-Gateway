# Architecture

## Runtime model

ESP32-C3 is single-core, so priorities and bounded work matter more than the
number of tasks. The radio worker runs at FreeRTOS priority 4. Wi-Fi, SNTP, and
backend workers run at priority 1; the Arduino loop serves the dashboard.
FreeRTOS mutex priority inheritance bounds contention around the small queue
and radio critical sections.

```text
                         DIO0 ISR
                            |
                    direct task notify
                            v
                  radio worker (priority 4)
       decode -> pairing -> dedup -> durable append -> ACK
                                                    -> RX restart
                            |
                            v
                      LittleFS queue
                            |
                backend worker (priority 1)
             Wi-Fi + trusted time + Bearer token
                            |
                   HTTPS JSON batches

  Wi-Fi worker --- STA reconnect and fallback AP
  SNTP worker ---- explicit trusted/untrusted UTC state
  Arduino loop --- local WebServer, dashboard, configuration, OTA
```

The ISR does not access SPI, parse packets, write flash, or log. It only wakes
the high-priority worker. DNS, NTP, HTTP, dashboard rendering, and backend
backoff never occur in the radio task.

## Receive and ACK invariant

For a new packet from the paired Node, the production order is:

1. RadioLib reports a completed receive and validates SX1278 packet CRC.
2. The Gateway independently decodes exact Protocol v1 framing.
3. The configured paired Node ID is checked.
4. The exact `(nodeId, bootSessionId, sequence)` identity is checked against
   queued and recently uploaded records.
5. A checksummed record is written to a temporary LittleFS file, flushed,
   closed, and atomically renamed to its committed name.
6. Only after that succeeds is the matching ACK transmitted.
7. ACK completion is finalized and continuous receive mode is restored before
   logging or any backend work.

If step 5 fails, the Gateway does not ACK. A duplicate is not appended again,
but it is ACKed again. Unknown and unconfirmed pairing candidates are neither
queued nor ACKed.

Diagnostics record queue-write duration, RX-to-durable-completion,
RX-to-ACK-start, and RX-to-ACK-complete latency in microseconds. The Node
timeout is 1,800 ms; normal operation is designed to remain far below it.

## Concurrency and failure isolation

Queue startup recovery completes before radio receive starts. Runtime `peek`
and terminal dequeue operations are limited to the configured maximum batch of
50 small files, rather than an unbounded scan. Startup is the only full spool
scan. There is no compaction in the receive path.

Lack of Wi-Fi, DNS, token, SNTP, or Backend does not prevent local receive,
durable append, or ACK. HTTPS uploads wait for trusted time; explicitly
configured local `http://` endpoints can be used without SNTP during HIL.

OTA necessarily interrupts capture during flash writes and reboot. The
dashboard and log state say `OTA IN PROGRESS`; no capture guarantee is made in
that window.

## Configuration and identity

One versioned configuration blob is validated before NVS persistence. Radio
changes are tested against the SX1278 first and rolled back in RAM if
initialization or RX restart fails; NVS is updated only after successful apply.
The immutable hardware MAC and editable logical Gateway ID are separate.
Default logical IDs are `GTH-GW-<12 uppercase MAC hex digits>`.
