# One-OS — Wireshark Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved only for the deduplicated protocol-parser scope below.**

Read root `README.md`, `docs/research/wireshark-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

Wireshark owns only bounded **wire-bytes → structured protocol fields** parsing needed by the product.

Use `wireshark_*` names.

## Approved implementation scope

Implement these production parsers first:

```c
wireshark_wifi_mgmt_parse(...);
wireshark_wifi_ie_parse(...);      // may be internal/helper if public separation is not useful
wireshark_ble_adv_parse(...);
```

Wi-Fi parser should cover the bounded fields needed by Kismet/Application/Device DB, such as:

- management subtype;
- source/destination/BSSID;
- SSID/hidden state;
- channel;
- capability bits;
- RSN/AKM/cipher summary;
- selected HT/HE metadata;
- bounded vendor IE descriptors;
- malformed/truncated flags.

BLE parser should cover generic AD structures such as:

- flags;
- local name;
- service UUIDs;
- service data;
- manufacturer data;
- TX power;
- appearance;
- malformed/truncated state.

## Explicitly do NOT implement

Do not implement in this product phase:

- DNS/mDNS parser (HA owns mDNS discovery);
- SSDP/DHCP parser if not directly required by the final app path;
- Zigbee parser (zigpy/ZHA own Zigbee semantics);
- Thread parser (OpenThread owns Thread semantics);
- ATT/GATT controller/parser framework (ESPHome GATT owns interaction);
- IEEE 802.15.4 MAC packet-inspector API solely for diagnostics;
- a universal dissector engine/filter VM/plugin system;
- device recognition/matching.

If a future concrete app need requires another parser, leave it for later approval rather than expanding scope now.

## Architecture rules

- Parser APIs operate only on caller-provided bytes/native metadata and return bounded copied results.
- No calls to Kismet, HA, ESPHome, zigpy, ZHA, OpenThread, Matter or other L2 families.
- No generic `nearby_*` compatibility layer.
- Device matching belongs only to the application Device DB.
- Wireshark-derived field behavior is reference/provenance; respect licensing and use clean-room/reference-only implementation where required.

## Implementation requirements

- strict length/endian/bitfield validation;
- deterministic OK/PARTIAL/MALFORMED/UNSUPPORTED/TRUNCATED semantics;
- no packet-driven unbounded heap allocation;
- fixed caps for UUIDs/vendor IEs/service fields;
- golden vectors and malformed/truncated vectors;
- fuzzable pure parser entry points where possible;
- host tests plus ESP32-C6 build validation;
- provenance notes for every field group.

## Safety

Normal protocol parsing/diagnostics are in scope. Do not add parsers/workflows whose product purpose is credential harvesting, security bypass, exploit delivery or session hijacking.

## Completion

Implement, test and commit the approved Wi-Fi management/IE and BLE AD parsers, then report APIs, parsed fields, test/fuzz coverage, build status and measured code-size impact if available.
