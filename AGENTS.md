# One-OS — Matter / CHIP Level-2 API Finalization Agent

## Status

Phase 1 research is complete and the Matter implementation is already substantially present on this branch. This is a **takeover/finalization task**, not a restart.

Current known candidate before takeover:

- branch: `research/matter-chip-tool-l2-api`
- candidate commit: `391f54b6862feb686dbd38a8cb439c94844d4c36`
- bounded host tests: passed
- ownership/security scope check: passed
- ESP32-C6 build: failed
- physical hardware validation: **not required**

The other eight approved L2 families have already been integrated into `main`. Before modifying Matter code, synchronize this branch with the latest `origin/main` and preserve those integrated components and their configuration.

Read root `README.md`, this file, `docs/research/matter-chip-tool-l2-api.md`, and the canonical application docs on current `main`:

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Immediate takeover tasks

1. Synchronize latest `origin/main` into this research branch. Resolve shared `CMakeLists.txt`, `sdkconfig.defaults`, partition, CI, and `main` conflicts by preserving the already-integrated eight-family main behavior plus only the Matter additions actually required.
2. Do **not** replace, remove, wrap, or refactor HA, ESPHome, Theengs, ZHA/zigpy, OpenThread, Kismet, Nmap, or Wireshark while fixing Matter.
3. Reproduce the Matter ESP32-C6 CI failure against the repository's real baseline: ESP-IDF v6.1 / ESP32-C6.
4. Continue fixing compile/link errors until `idf.py build`, `idf.py size`, and `idf.py size-components` complete successfully. Do not stop after the first compiler error to ask for approval.
5. Keep the One-OS ESP-IDF baseline at v6.1 unless the user explicitly approves changing it.
6. The currently pinned esp-matter revision documents ESP-IDF v6.0.2 as its supported/recommended baseline. Therefore do not assume that revision is compatible with v6.1. Determine a supported or minimally adapted esp-matter/connectedhomeip integration that actually compiles with One-OS v6.1. If changing the pinned upstream revision is necessary, document exactly why and keep the change pinned and reproducible.
7. Do not silently switch to an arbitrary Matter development branch merely because it builds. Preserve the approved Matter scope and report the selected upstream revision and compatibility rationale.

## Approved Matter ownership

Matter/CHIP owns protocol/controller operations **after the application has identified a Matter candidate**.

Owned here:

- controller initialization/shutdown and persisted single-controller/fabric state;
- CASE path for already-authorized operational nodes;
- bounded node interrogation;
- Interaction Model Read;
- Write;
- Invoke;
- Subscribe/Unsubscribe;
- explicit user-triggered commissioning workflows.

Target public APIs include:

```c
chip_controller_init(...);
chip_controller_shutdown(...);
chip_controller_get_fabric(...);
chip_request_cancel(...);
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

Do not implement generic BLE scanning, generic mDNS/DNS-SD scanning, a duplicate Matter candidate scanner/matcher, Thread network management, HA Device/Entity logic, or a separate Matter recognition DB.

No Matter L2 code may call or depend on another One-OS L2 family.

## Preserve and finish the existing implementation

Do not throw away the current candidate merely because CI fails. Inspect and repair it.

The existing implementation already contains controller/fabric handling, CASE-based operations, bounded `ReadClient` node probing, Read/Write/Invoke, subscriptions and commissioning. Keep real connectedhomeip/esp-matter code paths; do not replace them with placeholders, fake success, or test-only stubs.

### Bounded memory requirement

`matter_node_probe()` must remain fixed-capacity/bounded.

- Do not use `BufferedReadCallback` or application-owned unbounded `std::vector` whole-list accumulation.
- Prefer lower-level `ReadClient::Callback` streaming directly into fixed-capacity results.
- On overflow, return partial/truncated state rather than growing memory.
- Upstream internal temporary CHIP allocations may exist; document them rather than duplicating unbounded accumulation in One-OS.

### Cancellation and secret handling fixes

Review and fix the current candidate's completion/cancellation semantics before finalizing:

- `chip_request_cancel()` and commissioning cancellation must produce exactly one terminal user callback with `CHIP_STATUS_CANCELLED` where a callback has been accepted.
- Timeout, cancellation, CASE failure, IM failure and normal completion must not double-call callbacks or prematurely reuse a slot while an upstream transaction can still call back.
- Commissioning cancellation must not simply stop pairing and silently drop the caller callback.
- Explicitly clear sensitive temporary buffers when they are no longer needed, including stored Wi-Fi commissioning passwords, Thread operational datasets, IPK/NOC-related scratch material where applicable.
- Never log credentials, setup PIN material, IPK, private CA material, Wi-Fi password or Thread dataset bytes.

## Build hygiene

Do not solve Matter compatibility by weakening the entire One-OS compiler configuration.

In particular, do not leave repository-wide suppressions such as:

```text
-Wno-format-security
-Wformat=0
```

If an upstream CHIP target requires a warning workaround, scope it as narrowly as possible to the Matter/CHIP target and document it.

Do not replace main's combined eight-family CI with a Matter-only CI. Extend/preserve combined host tests and ESP32-C6 build validation so the final branch proves Matter coexists with the other integrated families.

## Physical hardware is not a completion gate

Do **not** wait for or request:

- Waveshare ESP32-C6 hardware;
- serial access;
- a pre-provisioned controller Fabric;
- a live Matter target;
- live CASE/ACL interoperability data;
- runtime free-heap/HWM measurements.

Unavailable runtime-only values must be reported as `NOT_MEASURED`. They are not blockers.

The completion gate is software-only:

- latest main synchronized;
- current Matter implementation reviewed/fixed;
- bounded host/unit tests pass;
- ownership/security scope checks pass;
- real ESP-IDF v6.1 / ESP32-C6 compile and link pass;
- `idf.py size` and `idf.py size-components` complete;
- combined main + Matter CI passes;
- no branch-specific `AGENTS.md` or temporary CI trigger should be proposed for main integration.

## Safety

Only authorized Matter sessions, reads, writes, subscriptions and explicit commissioning are in scope. No credential theft, authentication/attestation bypass, session hijacking, exploit delivery, unauthorized persistence or destructive behavior.

## Completion and handoff

Work continuously through build errors until the software completion gate is satisfied or there is a concrete upstream/toolchain blocker that cannot be resolved without changing a user-approved architecture decision.

Commit all fixes on `research/matter-chip-tool-l2-api`, then report:

- final commit SHA;
- selected esp-matter and connectedhomeip revisions;
- APIs implemented;
- host/unit/scope test results;
- ESP-IDF v6.1 / ESP32-C6 build result;
- `idf.py size` / `size-components` results;
- runtime-only values as `NOT_MEASURED` when unavailable;
- any remaining blocker with the exact compiler/linker/API error.

Do **not** merge this branch into `main` yourself. Final main integration is performed separately after review.