# One-OS — OpenThread Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved for the deduplicated Thread network-layer scope below.**

Read root `README.md`, `docs/research/openthread-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

OpenThread owns only Thread network operations:

- network discovery;
- local Thread state snapshot;
- local topology/neighbor/router information;
- authorized dataset attach/joiner workflows.

Matter device semantics/control belong to Matter/CHIP and must not be duplicated here.

Use `openthread_*` names.

## Approved implementation scope

Implement first:

```c
openthread_discover_networks(...);
openthread_get_state_snapshot(...);
```

Then, if the first slice is stable and resource-safe:

```c
openthread_get_local_topology(...);
openthread_attach_dataset(...);
openthread_joiner_join(...);
```

Discovery/state results should include bounded copies of useful fields such as channel, PAN ID, Extended PAN ID, network name, RSSI/LQI, role/state and local topology metadata where available.

## Explicitly do NOT implement

- Matter endpoint/cluster/device control;
- generic IEEE 802.15.4 packet scanner/parser;
- BLE/Wi-Fi scanning;
- Border Router product architecture unless separately approved later;
- HA Device/Entity semantics;
- a separate recognition database.

Thread networks are infrastructure evidence; do not automatically materialize every Thread network as a controllable HA Device.

## Architecture rules

- Use native ESP-IDF/OpenThread integration directly.
- No calls to Matter, Kismet, Wireshark, HA, ZHA/zigpy or other L2 families.
- No generic `nearby_*` abstraction.
- Application owns cross-family RF serialization.
- BLE scan, Zigbee ownership and Thread discovery must be serialized conservatively until target measurements prove a safe coexistence mode.

## Implementation requirements

- fixed/bounded scan/topology result storage;
- finite timeout/cancel for discovery/join flows;
- clean restore/attach/detach state handling;
- secure dataset handling; never log network keys;
- host/unit tests where feasible plus ESP32-C6 build validation;
- document actual memory/flash impact if measured.

## Safety

Authorized discovery, attach, joiner and diagnostics are in scope. No key extraction, unauthorized network entry, security bypass, jamming, hostile replay, hijacking or exploit delivery.

## Completion

Implement, test and commit the approved scope on this branch, then report APIs, build/tests, measured footprint if available, radio/state limitations and deferred items.
