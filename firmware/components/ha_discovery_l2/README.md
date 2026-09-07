# ha_discovery_l2

Finite, bounded Home Assistant-derived LAN discovery workflows for One-OS.

Implemented ownership is intentionally narrow:

- `ha_mdns_discover_once()` performs a one-shot mDNS/DNS-SD browse and returns
  normalized service type / instance / SRV host+port / A+AAAA / TXT evidence.
- `ha_ssdp_discover_once()` performs multicast M-SEARCH plus the Home Assistant
  IPv4 broadcast fallback and returns normalized case-insensitive SSDP headers.

The component does not contain Home Assistant matcher tables, Device DB logic,
raw RF scanning, UPnP control, Matter/HomeKit control, or another project L2 API.
All packet-driven storage is fixed/caller-bounded and both workflows expose a
finite timeout and cancellation callback.

Provenance: behavior is a clean-room embedded reimplementation informed by the
Phase-1 report and Home Assistant Core discovery behavior. Wire parsing follows
mDNS/DNS-SD and SSDP protocol formats; no `python-zeroconf` or
`async-upnp-client` code is copied.
