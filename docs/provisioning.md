# Wi-Fi and backend provisioning

## Fallback access point

If there is no configured SSID, the Gateway immediately starts:

```text
SSID:     GATHRA-GW-<last 6 MAC hex digits>
Credential: firmware-defined WPA2 credential
URL:      http://192.168.4.1/
```

The AP uses WPA2 through the ESP32 Wi-Fi stack. There is no
additional dashboard login, so anyone on the AP or reachable LAN can
view diagnostics and change configuration. Treat network reachability as a
security boundary.

## STA lifecycle

Enter an SSID and password in the local dashboard. Password fields use replace
semantics: blank retains the stored value and the explicit checkbox clears it.
Status JSON never returns the stored password.

Default policy:

- STA reconnect attempt every 15 seconds.
- fallback AP after 60 seconds without a stable STA connection;
- AP shutdown 30 seconds after STA becomes stable;
- clearing credentials cancels an in-progress driver connection immediately
  and leaves the fallback AP available;
- AP returns after a later extended disconnect.

The dashboard remains available on the STA LAN address. Best-effort mDNS is
`http://gathra-gateway.local/`, but direct IP access is authoritative.

## Backend

Default base URL is `https://api.gathra.my.id`. Provision the raw Gateway
Bearer token once; status reports only whether one exists. Blank retains and
the explicit checkbox clears it. Tokens are never logged or returned.

HTTPS uploads require trusted SNTP time and validate the server hostname and
certificate chain using the embedded GTS Root R4. An isolated HIL setup may use
`http://<reachable-test-host>:3000`; dashboard diagnostics label it
`LOCAL_HTTP` rather than validated HTTPS. Production delivery uses HTTPS over
the STA connection.
