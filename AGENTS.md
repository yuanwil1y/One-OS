# One-OS — Matter / CHIP Level-2 API Finalization Agent

## Status

This is a **takeover/finalization task**, not a restart.

Current branch state at handoff:

- branch: `research/matter-chip-tool-l2-api`
- current HEAD: `e3bbe157bb934b7d94508d7a80afa9b30cce0e78`
- latest `main`: already merged into this branch; current branch is ahead of `main` and not behind it
- esp-matter pin: `espressif/esp-matter@b5dd92663ba67dcac214fe964b56b0bb87333939` from `release/v1.6.1`
- connectedhomeip revision: `539342f32d...` as pinned by that esp-matter revision
- all combined host tests for HA, ESPHome, Theengs, ZHA/zigpy, OpenThread, Kismet, Nmap, Wireshark and Matter: **PASS**
- Matter ownership/security scope check: **PASS**
- latest ESP-IDF v6.1 / ESP32-C6 build: **FAIL**
- latest failing workflow run: `34156699845`
- physical hardware validation: **not required**

The previous takeover already did two useful things:

1. synchronized the Matter branch with the integrated eight-family `main`;
2. removed repository-wide format-warning suppression during conflict resolution and pinned esp-matter to the immutable `release/v1.6.1` head.

Do not redo those steps unless `main` has actually moved again.

Important: esp-matter `release/v1.6.1` still documents/recommends ESP-IDF `v6.0.2`. Therefore the new pin is **not proof of ESP-IDF v6.1 compatibility**. The remaining job is to diagnose and fix the real v6.1 compile/link incompatibility, not to keep changing versions blindly.

Read root `README.md`, this file, `docs/research/matter-chip-tool-l2-api.md`, and the canonical application docs on current `main` before editing code:

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Immediate takeover tasks

1. Start from current HEAD. Do not rewrite the Matter component from scratch.
2. Inspect the latest failing ESP32-C6 workflow `34156699845` and capture the **first real compiler/linker error**, including the source file, symbol/API and diagnostic text.
3. Reproduce/fix errors against the repository baseline: **ESP-IDF v6.1 + ESP32-C6**.
4. Iterate continuously through all subsequent compile/link errors until `idf.py build`, `idf.py size`, and `idf.py size-components` succeed. Do not stop after one or two fixes to ask for approval.
5. Keep One-OS on ESP-IDF v6.1. Do not downgrade the project to v6.0.2.
6. Do not keep repinning esp-matter without evidence. The current `release/v1.6.1` pin is acceptable as a starting point. Change it only if the actual compiler/API evidence shows that a different **pinned, reproducible, non-arbitrary** upstream revision is necessary for v6.1.
7. If an upstream API changed between the esp-matter-supported IDF baseline and IDF v6.1, prefer a narrow compatibility adaptation in the Matter integration over repository-wide compiler weakening or modification of unrelated L2 families.
8. Preserve all eight already-integrated L2 families and their combined tests/configuration.

## Approved Matter ownership

Matter/CHIP owns only protocol/controller operations after the application identifies a Matter candidate:

- controller initialization/shutdown and persisted single-controller/fabric state;
- CASE path for already-authorized operational nodes;
- bounded node interrogation;
- Interaction Model Read;
- Write;
- Invoke;
- Subscribe/Unsubscribe;
- explicit user-triggered commissioning.

Public APIs already present include:

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

Do not implement generic BLE scanning, generic mDNS/DNS-SD scanning, Matter candidate matching, Thread network management, HA Device/Entity logic, or a separate recognition DB.

No Matter L2 code may call or depend on another One-OS L2 family.

## Preserve the existing implementation

The existing `firmware/components/matter_l2/` candidate already contains real connectedhomeip/esp-matter code for Controller/Fabric, CASE, bounded `ReadClient` Node Probe, Read, Write, Invoke, Subscribe and Commissioning.

Do not replace it with placeholders, fake success paths, mock production code, or a Linux `chip-tool` port.

### Bounded memory

`matter_node_probe()` must remain fixed-capacity and streaming:

- no `BufferedReadCallback` whole-list buffering;
- no application-owned unbounded `std::vector` accumulation;
- stream list elements through `ReadClient::Callback` into fixed-capacity result structures;
- overflow returns partial/truncated state.

### Cancellation and sensitive buffers still require review

The current source still needs final review/fix for these semantics while build compatibility is being repaired:

- commissioning cancellation must deliver exactly one terminal `CHIP_STATUS_CANCELLED` callback after a request has been accepted;
- cancellation/timeout/failure/success paths must not double-call callbacks;
- request/subscription slots must not be reused while an upstream transaction can still callback into them;
- clear the full stored Wi-Fi password buffer when commissioning state is reset/finished;
- clear the full Thread operational dataset buffer when reset/finished;
- clear IPK/NOC/other credential scratch buffers where applicable after use;
- never log setup PINs, Wi-Fi passwords, Thread datasets, IPK, private CA material or credential bytes.

Do not postpone these fixes merely because they are not the first compile error.

## Build hygiene

Do not fix Matter by weakening the whole repository.

Forbidden repository-wide workarounds include:

```text
-Wno-format-security
-Wformat=0
```

If a specific upstream CHIP source genuinely needs a warning workaround, scope it to the narrowest target and document why.

Do not replace the combined CI with Matter-only validation. Final Matter branch CI must keep all previously integrated host tests plus Matter tests.

## Hardware is not a completion gate

Do not wait for or request:

- Waveshare hardware;
- serial access;
- a live Matter device;
- a pre-provisioned Fabric;
- live CASE/ACL validation;
- free-heap or task-HWM measurements.

Unavailable runtime values are `NOT_MEASURED`, never blockers.

## Completion gate

Finish only when all software conditions are satisfied:

- branch still includes current `main`;
- existing Matter APIs remain implemented with real SDK paths;
- all combined host tests pass;
- Matter scope/security check passes;
- ESP-IDF v6.1 / ESP32-C6 compile and link pass;
- `idf.py size` and `idf.py size-components` pass;
- cancellation and sensitive-buffer issues above are resolved;
- no global compiler-security weakening is introduced.

Commit all fixes on `research/matter-chip-tool-l2-api` and report:

- final commit SHA;
- esp-matter and connectedhomeip revisions;
- the concrete build incompatibilities fixed;
- host/scope test results;
- ESP32-C6 build result;
- size/size-components result;
- runtime-only values as `NOT_MEASURED`;
- any truly unresolved blocker with the exact diagnostic.

Do not merge this branch into `main` yourself.