# nmap_l2

Bounded active LAN host, TCP port, and small service discovery for One-OS.

This component is an independent clean-room implementation of a small subset of Nmap-like scanner semantics. It does **not** embed Nmap source, Nmap databases, NSE, mDNS, SSDP, OS fingerprinting, raw SYN scanning, or a device-recognition database.

## Runtime prerequisites

All Runtime entry points require the default `WIFI_STA_DEF` netif to have a valid IPv4 address and netmask. Initial P0 scans are restricted to the directly connected IPv4 subnet.

`use_local_subnet=true` is intentionally narrower: only classic `/24` through `/30` subnets are auto-enumerated. Wider subnets must be supplied as an explicit bounded target list by the application.

Only one Nmap-family scan may be active at a time. This is a family-internal resource bound, not a generic application scan coordinator.

## Public workflow

```c
nmap_discovery_start(...);
nmap_discovery_cancel(...);
nmap_discovery_wait(...);

nmap_port_scan_start(...);
nmap_port_scan_cancel(...);
nmap_port_scan_wait(...);

nmap_service_scan_start(...);
nmap_service_scan_cancel(...);
nmap_service_scan_wait(...);
```

Results are streamed through caller callbacks. Callbacks are serialized by the component and must return quickly; copy any record that must outlive the callback.

Every successful `start()` must eventually be paired with the matching `wait()`, including after cancellation, before another Nmap-family scan can start.

## Hard bounds

| Resource | Bound |
|---|---:|
| Concurrent workers | 4 |
| Retry count | 1 |
| Probe rate | <= 50/s |
| Probe timeout | 50..3000 ms |
| Whole-scan timeout | 100..120000 ms |
| Host discovery targets | 254 |
| TCP fallback ports per host | 4 |
| Port-scan targets | 32 |
| Ports per port scan | 32 |
| Total TCP port jobs | 512 |
| Service endpoints | 64 |
| Service response capture | 32..512 bytes |

`nmap_timing_policy_default()` returns the conservative initial policy: 4 workers, one retry, 20 probes/s, 400 ms per probe and 30 s whole-scan deadline.

## Host discovery

Host discovery can combine one bounded ICMP Echo attempt (using ESP-IDF `esp_ping_*`) with TCP connect fallback ports. The built-in fallback set when the caller supplies none is `80, 443, 22`; this is a One-OS product convenience set, not Nmap top-port data.

Evidence semantics are conservative:

- ICMP reply => host up.
- TCP connect accepted => host up.
- TCP `ECONNREFUSED` => host up even though that port is closed.
- timeout alone => `NMAP_HOST_NO_RESPONSE`, never definitive down.
- network/host unreachable errors remain distinct.

## TCP port discovery

Port discovery uses non-blocking BSD sockets, `select()` and `getsockopt(SO_ERROR)`. It normalizes native socket outcomes into `OPEN`, `CLOSED`, `FILTERED`, `UNREACHABLE`, or `ERROR` plus a separate reason.

No raw TCP packet construction is used.

## Service discovery

P0 service discovery is deliberately small:

- passive SSH identification string;
- passive FTP greeting only when the banner explicitly identifies FTP;
- passive SMTP greeting only when the banner contains conservative SMTP-family evidence;
- one bounded HTTP `HEAD / HTTP/1.0` request for explicit HTTP probing and the small AUTO HTTP port set (`80`, `8000`, `8080`, `8888`).

Unknown or generic banners remain unknown. A conventional port number alone never becomes service identity.

The service record includes bounded service/product/banner evidence and a `probe_id` suitable for copying into application Device DB matching input.

See `PROVENANCE.md` for every built-in probe/match rule.
