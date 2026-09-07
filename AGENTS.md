# One-OS — ESPHome Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved only for the deduplicated scope below.**

Read root `README.md`, `docs/research/esphome-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

ESPHome owns only:

1. **generic BLE GATT device interaction workflow**;
2. **ESPHome Native API interrogation/state/control**.

Use `esphome_ble_gatt_*` and `esphome_api_*` names.

## Approved implementation scope

Implement bounded GATT workflows equivalent in capability to:

```c
esphome_ble_gatt_connect(...);
esphome_ble_gatt_discover(...);
esphome_ble_gatt_read(...);
esphome_ble_gatt_write(...);
esphome_ble_gatt_subscribe(...);
esphome_ble_gatt_disconnect(...);
```

The API must add real lifecycle/orchestration value: connection timeout, service/characteristic discovery, bounded copied results, subscription lifecycle, error cleanup and native-state restoration. Do not wrap a single NimBLE call just to rename it.

Then implement the smallest practical ESPHome Native API vertical slice, prioritizing:

```c
esphome_api_probe(...);
esphome_api_entities(...);
esphome_api_subscribe(...);
esphome_api_command(...);
```

If protobuf/Noise footprint blocks the full slice, first land the bounded transport/probe foundation and document the measured blocker. Do not fake unsupported behavior.

## Explicitly do NOT implement

Do not implement in this family:

- BLE RF scanning/tracking;
- BLE advertisement AD parsing;
- mDNS discovery;
- BTHome/Xiaomi/Ruuvi passive decoders;
- separate ESPHome device matching database;
- generic hardware fingerprint matching;
- protocol-specific UI.

Those unique owners are Kismet, Wireshark, HA, Theengs/Device DB and the application respectively.

## Architecture rules

- L1 remains native ESP-IDF/NimBLE/lwIP/FreeRTOS/BSP.
- No cross-family dependency: do not call Kismet, Wireshark, HA, Theengs, Nmap, zigpy, ZHA, OpenThread or Matter APIs.
- Device matching is performed by the single application Device DB; ESPHome receives an already selected target/profile/binding from the application.
- No generic `nearby_*` compatibility layer.
- Credentials/Noise keys are caller/application supplied and never guessed or logged.

## Implementation requirements

- fixed/bounded service/characteristic/subscription/result storage;
- explicit timeout/cancel/disconnect cleanup;
- malformed/oversized payload handling;
- tests with mock/fixture GATT and Native API messages where possible;
- continuously buildable ESP32-C6 tree;
- provenance notes and previous-project reuse notes.

## Safety

Authorized BLE connection/read/write/notify and authenticated ESPHome control are in scope. No credential theft, auth bypass, hostile MITM, session hijacking, exploit delivery or persistence.

## Completion

Implement, test and commit the approved scope on this branch, then report APIs, files, build/tests, measured footprint if available, and remaining limitations.
