# Wi-Fi and backend provisioning

## Fallback access point

If there is no configured SSID, the Gateway immediately starts:

```text
SSID:     GATHRA-GW-<last 6 MAC hex digits>
Password: sman35jakarta
URL:      http://192.168.4.1/
```

The AP uses WPA2 through the ESP32 Wi-Fi stack. There is deliberately no
additional dashboard login in v1, so anyone on the AP or reachable LAN can
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
certificate chain using the embedded GTS Root R4. Local HIL may deliberately
use `http://<reachable-host-ip>:3000`; dashboard diagnostics label it
`LOCAL_HTTP` rather than validated HTTPS. For an isolated test, a Backend on a
host associated with the fallback AP is reachable at that host's AP-side IP
(normally `http://192.168.4.2:3000`) even when Gateway STA is not connected;
production HTTPS uploads remain STA-only.
