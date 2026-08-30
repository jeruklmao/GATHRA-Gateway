# Gateway-to-Backend contract

## Authentication and transport

The Gateway sends `Authorization: Bearer <gateway-token>`. The raw token exists
only in Gateway NVS; the Backend is configured with its SHA-256 digest. The
production base URL is `https://api.gathra.my.id`, with normal certificate and
hostname validation. No production code disables TLS verification.

Compatibility can be checked without touching the queue:

```http
GET /api/v1/iot/gateway/ping
Authorization: Bearer <gateway-token>
```

## Batch request

```http
POST /api/v1/iot/telemetry/batch
Content-Type: application/json
Authorization: Bearer <gateway-token>
```

```json
{
  "schemaVersion": 1,
  "gateway": {
    "gatewayId": "GTH-GW-AABBCCDDEEFF",
    "hardwareMac": "AA:BB:CC:DD:EE:FF",
    "firmwareVersion": "2.2.0",
    "bootSessionId": 1234567890
  },
  "readings": [
    {
      "gatewayReceivedAt": "2026-08-18T05:00:00.123Z",
      "gatewayTimeTrusted": true,
      "gatewayUptimeMs": 123456,
      "gatewayBootSessionId": 1234567890,
      "rssiDbm": -91.5,
      "snrDb": 8.25,
      "frequencyErrorHz": -731,
      "packetLength": 64,
      "rawPayloadBase64": "R1QDAQJOMQECAwSgsMDQAAASNAAAAuQAAALjAAP7LhHXDnQHBwAAAwICAABpq83vCgFpq/AAAQIDBQMAAAAF3A=="
    }
  ]
}
```

`gateway.bootSessionId` describes the uploader's current process.
`readings[].gatewayBootSessionId` is the capture-time identity and is persisted
per reading, including when a rebooted Gateway uploads records captured during
a different boot.

When UTC was not trusted at capture, the pair must be exactly:

```json
{
  "gatewayReceivedAt": null,
  "gatewayTimeTrusted": false
}
```

No fake epoch timestamp is sent. Batch default is 20 and maximum is 50.

## Response and queue policy

```json
{
  "receivedAt": "2026-08-18T05:01:00.000Z",
  "results": [
    {
      "index": 0,
      "nodeId": "N1",
      "bootSessionId": 16909060,
      "sequence": 2695938256,
      "status": "INSERTED"
    }
  ]
}
```

Only HTTP 200 with validated per-index terminal results permits dequeue.
`INSERTED`, `DUPLICATE`, and `REJECTED_INVALID` are terminal for that index.
Missing indices stay queued. Authentication errors, redirects, 4xx batch
envelope failures, 5xx, DNS/TLS errors, timeouts, and malformed responses keep
records for bounded exponential retry.

The initial retry delay is 2 seconds, doubles to a 5-minute cap, and adds up to
20% random positive jitter. There is no busy loop.

The Backend independently decodes the raw packet; it does not accept decoded
sensor values from the Gateway.

## Operational heartbeat

Firmware 2.2.0 uses the same base URL, bearer token, CA trust, and
hostname verification for:

```http
POST /api/v1/iot/gateway/heartbeat
```

This best-effort request is never queued and any 2xx response is terminal. Its
independent HTTP schema version is 1; LoRa remains Protocol 3. The complete
field contract, nullable rules, timing definitions, and reset semantics are in
`gateway-heartbeat.md`.
