# One-OS — ZHA / zigpy Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved for the deduplicated Zigbee scope below.**

Read root `README.md`, `docs/research/zha-zigpy-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

Keep the two upstream families separate.

### zigpy owns Zigbee protocol/controller workflow

Implement capabilities equivalent to:

```c
zigpy_commissioning_start(...);
zigpy_commissioning_stop(...);
zigpy_interview_begin(...);
zigpy_interview_cancel(...);
zigpy_interview_get_snapshot(...);
zigpy_attr_read_async(...);
zigpy_attr_write_async(...);
zigpy_command_invoke_async(...);
zigpy_reporting_configure_async(...);
```

The important product path is:

```text
existing/authorized Zigbee device
→ interview
→ endpoint/cluster inventory
→ attribute read/write
→ command
→ reporting
```

### ZHA owns selected quirk/capability semantics

The application Device DB is the only Runtime fingerprint matcher. It returns a selected `zha_quirk_id`.

Implement APIs equivalent to:

```c
zha_quirk_apply_by_id(...);
zha_capability_enumerate(...);
zha_transform_decode(...);
zha_transform_encode(...);
zha_action_decode(...);
```

ZHA converts selected vendor quirks/cluster semantics into normalized capabilities that the application maps to HA Entities.

## Explicitly do NOT implement

- ZHA Runtime full-corpus fingerprint search (`zha_quirk_match()` must not become a second matcher DB).
- raw IEEE 802.15.4 generic scanner.
- Thread/Matter operations.
- protocol-specific UI.
- zigpy calling ZHA or ZHA calling zigpy.

All ZHA/zha-device-handlers fingerprint data belongs in the single Device DB generator/corpus. The application copies bounded data between zigpy and ZHA.

## Architecture rules

- L1 remains native ESP Zigbee/802.15.4/FreeRTOS facilities.
- `zigpy_*` and `zha_*` remain independent public families.
- No cross-family One-OS dependencies.
- No generic `nearby_*` compatibility layer.
- Commissioning/permit-join is explicit user action and finite.
- Existing joined devices may be inventoried during a normal scan; normal scan must not silently open permit-join.

## Implementation requirements

- bounded endpoints/clusters/attributes/results;
- explicit timeout/cancel/partial interview state;
- persistent Zigbee network/controller state handled safely;
- read/write/command/report errors surfaced explicitly;
- selected quirk tables/data have provenance and deterministic IDs;
- host tests for transforms/quirks plus target build coverage;
- ESP32-C6 build stays green.

## Safety

Authorized join/interview/read/write/report/command workflows are in scope. No key extraction, unauthorized network entry, security bypass, jamming, hostile replay, exploit delivery or hijacking.

## Completion

Implement, test and commit the approved zigpy + ZHA scope, then report APIs, supported capabilities, test/build results, measured footprint if available and remaining limitations.
