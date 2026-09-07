# One-OS — Matter / CHIP Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved only for the deduplicated Matter controller scope below.**

Read root `README.md`, `docs/research/matter-chip-tool-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

Matter/CHIP owns only protocol/controller operations **after the application has identified a Matter candidate**.

It does not own generic BLE or mDNS environment scanning.

Use `chip_*` for controller/CHIP-style operations and `matter_*` for protocol-semantic helpers such as node probing.

## Approved implementation scope

Implement in this order:

1. **controller footprint/feasibility spike on ESP32-C6**;
2. persisted single-controller/fabric foundation if feasible;
3. secure session to an already authorized node;
4. bounded node interrogation;
5. Interaction Model read support;
6. then write/invoke/subscribe if footprint remains acceptable;
7. commissioning only after the previous stages are proven.

Target APIs/capabilities include:

```c
chip_controller_init(...);
chip_controller_shutdown(...);
matter_node_probe(...);
chip_read_attribute(...);
chip_write_attribute(...);
chip_invoke(...);
chip_subscribe_start(...);
chip_subscribe_stop(...);
chip_commission_onnetwork(...);
chip_commission_ble_wifi(...);
chip_commission_ble_thread(...);
```

Do not invent APIs that cannot actually fit/run. If controller footprint blocks later stages, land the proven subset and document measured limits.

## Explicitly do NOT implement

- generic BLE scanning;
- generic mDNS/DNS-SD scanning;
- duplicate Matter candidate scanning pipeline;
- Thread network management (OpenThread owns that);
- HA Device/Entity logic;
- a separate Matter recognition DB.

Matter candidates arrive from the application through Kismet/Wireshark BLE evidence or HA mDNS evidence plus Device DB matching. The Matter family then takes over protocol operations.

## Architecture rules

- No calls to HA, Kismet, Wireshark, OpenThread, ZHA or other L2 families.
- Use connectedhomeip/native ESP-IDF platform facilities directly.
- No generic `nearby_*` layer.
- Fabric credentials and commissioning material are secure persistent protocol state, never Device DB data.
- Commissioning is always an explicit user action; normal environment scan never commissions automatically.

## Implementation requirements

- measured flash/RAM/task-stack footprint for the controller spike;
- bounded endpoint/cluster/attribute results;
- explicit timeout/cancel/error state;
- secure credential handling and no secret logging;
- CASE/ACL/attestation failures handled fail-closed;
- tests where possible plus real ESP32-C6 build validation;
- provenance/version notes for connectedhomeip integration.

## Safety

Public discovery evidence may be consumed, but only authorized Matter sessions, reads, writes, subscriptions and commissioning are in scope. No security bypass, credential theft, attestation bypass, hijacking, exploit delivery or unauthorized persistence.

## Completion

Implement the largest proven subset in the approved order, commit it, and report APIs, exact footprint measurements if obtained, build/tests, what was deferred and why.
