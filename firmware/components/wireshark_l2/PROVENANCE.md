# wireshark_l2 provenance

This component implements only the approved bounded Wi-Fi management/IE and BLE advertising-data parsers.

Production code is a **CLEAN-ROOM REIMPLEMENT** from public protocol definitions:

- IEEE 802.11 management-frame, information-element, and RSN definitions.
- Bluetooth Core Specification GAP Advertising Data definitions and Bluetooth Assigned Numbers.

Wireshark source is **REFERENCE-ONLY** field-truth/test-oracle material. No Wireshark source code, `tvbuff_t`, `proto_tree`, dissector registry, display-filter VM, plugin runtime, reassembly/conversation framework, or project-specific Wireshark public type is copied or linked into One-OS.

Reference snapshot used during Phase-1 research: `wireshark/wireshark` GitHub mirror commit `cc2f545b2360075c2bf6f19c78e81c77c429399f`.

## Field groups

### BLE advertising

- AD TLV framing: Bluetooth GAP AD structure definition.
- Flags, local name, TX power, appearance: Bluetooth GAP data types.
- 16/32/128-bit service UUID lists and service data: Bluetooth GAP data types / Assigned Numbers.
- Manufacturer Specific Data company identifier: Bluetooth Assigned Numbers.
- Manufacturer/service payload bytes remain opaque and bounded; device-specific decoding belongs to another approved owner.

### Wi-Fi management / IEs

- Frame Control, management subtype, addresses, sequence control, beacon/probe fixed fields: IEEE 802.11.
- SSID, Supported/Extended Rates, DS Parameter Set, HT Capabilities/Operation, Extension IE: IEEE 802.11.
- RSN version/cipher/AKM/capability layout: IEEE 802.11 RSN definitions.
- Vendor-specific IE OUI/type/data is exposed only as a bounded descriptor; no vendor dissector universe is implemented.
- HE metadata is deliberately shallow (presence of HE Capabilities / HE Operation extension elements only).
