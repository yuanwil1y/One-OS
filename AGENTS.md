# One-OS — Matter / CHIP Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved for the deduplicated Matter controller scope below. Do not block implementation on unavailable physical hardware or a live Matter target.**

Read root `README.md`, `docs/research/matter-chip-tool-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

Matter/CHIP owns only protocol/controller operations **after the application has identified a Matter candidate**.

It does not own generic BLE or mDNS environment scanning.

Use `chip_*` for controller/CHIP-style operations and `matter_*` for protocol-semantic helpers such as node probing.

## Approved implementation scope

Implement the complete approved software API path without requiring real-device validation:

1. controller initialization/shutdown and single-controller/fabric persistence interfaces;
2. secure-session/CASE path for an already authorized node;
3. bounded node interrogation;
4. Interaction Model attribute read;
5. write;
6. invoke;
7. subscribe/unsubscribe;
8. commissioning workflows last.

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

Do not stop at an artificial hardware-validation gate. If a behavior cannot be exercised without a physical Matter node, implement it against the real connectedhomeip/esp_matter APIs, cover deterministic logic with host/unit tests where practical, compile/link it in the actual ESP32-C6 CI build, and clearly mark only the runtime interoperability observation as untested.

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

- The completion gate is **software-only**: real ESP32-C6 target build/CI, link success, host/unit tests where practical, bounded API design, and code review of memory behavior.
- Do **not** require a Waveshare board, serial port, pre-provisioned Fabric, live Matter target, CASE runtime connection, or measured runtime heap/task HWM to finish this branch.
- Report build-time/static footprint information when available (firmware size, linked component contribution, map-file observations). Do not invent runtime measurements.
- Runtime-only metrics that cannot be measured in the available environment must simply be reported as `NOT_MEASURED`; they are not blockers.
- bounded endpoint/cluster/attribute results;
- explicit timeout/cancel/error state;
- secure credential handling and no secret logging;
- CASE/ACL/attestation failures handled fail-closed;
- provenance/version notes for connectedhomeip integration.

### Bounded-read requirement

Do not implement `matter_node_probe()` by buffering arbitrarily large Matter Lists in application-owned dynamic containers.

If an upstream convenience helper uses `std::vector` or another whole-list accumulator, prefer the lower-level `ReadClient` callback path and stream list elements into fixed-capacity/caller-owned output records. On overflow, return partial/truncated status rather than growing without bound.

If connectedhomeip itself performs unavoidable internal temporary allocation, document that fact; only application-controlled unbounded accumulation must be eliminated.

## Version/build compatibility

- Validate the chosen esp_matter / connectedhomeip integration by compiling it in the repository's actual ESP32-C6 CI toolchain.
- Do not change the whole One-OS ESP-IDF baseline merely to make this branch easier unless explicitly approved by the user.
- If an upstream release is incompatible with the current IDF version, document the concrete build failure and select a compatible supported integration path where possible.
- A lack of physical hardware is never a reason to stop software implementation.

## Safety

Public discovery evidence may be consumed, but only authorized Matter sessions, reads, writes, subscriptions and commissioning are in scope. No security bypass, credential theft, attestation bypass, hijacking, exploit delivery or unauthorized persistence.

## Completion

Implement the approved Matter software scope as far as the actual SDK APIs and ESP32-C6 build permit, including read/write/invoke/subscribe and commissioning after the controller foundation is in place. Commit all buildable production code and tests on this branch, then report:

- APIs implemented;
- ESP32-C6 CI/build result;
- host/unit test result;
- static/build footprint information actually available;
- runtime metrics as `NOT_MEASURED` when hardware is unavailable;
- any remaining SDK/toolchain blockers.

Do not wait for or request real-device validation before completing and reporting this branch. Do not merge to `main` yourself.
