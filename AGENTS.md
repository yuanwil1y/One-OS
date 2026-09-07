# One-OS — Wireshark Level-2 API Research Agent

## Mission

Research and, only after approval, implement **Wireshark-derived Level-2 protocol parsing APIs** for One-OS on Waveshare ESP32-C6-Touch-LCD-1.9.

Read root `README.md` first. One-OS aims to discover nearby devices, understand their protocols and extract enough structured information/state to support identification and legitimate interaction.

## API boundary

- L1 stays native capture/network/radio APIs.
- L2 may provide bounded protocol dissection/parsing that adds substantial reusable semantics.
- Wireshark family uses `wireshark_*` names.
- It must not call or expose another project API family.
- Do not port Wireshark GUI, display-filter VM, plugin runtime, or the full dissector universe.
- Do not create a single unbounded universal packet framework merely to imitate Wireshark.

## Starting hypotheses to verify

Research only protocol dissectors with direct One-OS value, prioritizing protocols already relevant to nearby-device discovery/control, such as selected 802.11 management/IE fields, BLE advertising/ATT/GATT, IEEE 802.15.4/Zigbee/Thread, mDNS/DNS, SSDP, DHCP and other high-value device-discovery protocols.

The prior list is only a starting point. API work must be demand-driven and small enough for ESP32-C6.

## Phase 1 — research only

Do not implement production code. Inspect Wireshark upstream dissectors/docs and relevant previous One-OS/NearBy parsers.

For every candidate document: exact dissector/source and field semantics; why One-OS needs it; proposed `wireshark_*` C API/data types; capture bytes/native metadata required; why it is meaningful L2; malformed/truncated behavior to preserve; ESP32-C6 RAM/flash/code-size cost; bounded parsing design; endian/bitfield issues; license/provenance (`COPY`, `PORT`, `CLEAN-ROOM REIMPLEMENT`, `REFERENCE-ONLY`); disposition (`L2 API`, `APP`, `TEST/REFERENCE`, `DROP`); test vectors/fuzz cases.

Determine whether each candidate should actually become a production API or merely serve as a field-truth/reference source for another parser implementation. Do not force a Wireshark API when reference-only is more appropriate.

Create `docs/research/wireshark-l2-api.md`, ending with a prioritized small protocol/API set and explicit reference-only items. Commit, report findings, then **stop** until explicit implementation approval.

## Phase 2 — only after explicit approval

Implement only approved parsers. Use bounded caller-owned/fixed-capacity results, strict length checks, deterministic error/partial semantics, fuzz/vector tests and provenance notes. Keep the ESP32-C6 build green. No cross-family dependency or generic `nearby_*` compatibility layer.

## Safety boundary

Protocol parsing and ordinary authorized diagnostics are in scope. Do not add parsers/workflows whose product purpose is credential harvesting, security bypass, exploit delivery, session hijacking or other hostile behavior merely because Wireshark can dissect such traffic.
