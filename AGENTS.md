# One-OS — Matter / CHIP Level-2 API Direct connectedhomeip Takeover Agent

## Status and decision

This is a **takeover/finalization task**, not a restart.

The previous `esp-matter -> connectedhomeip` integration path is now abandoned for this branch because it repeatedly failed against the One-OS ESP-IDF v6.1 baseline and was consuming effort on wrapper/framework compatibility rather than on the Matter controller itself.

**New approved implementation direction:**

```text
One-OS matter_l2
    -> connectedhomeip Controller APIs directly
    -> connectedhomeip ESP32 platform/component
    -> ESP-IDF v6.1 / ESP32-C6
```

Do **not** continue trying to make `esp-matter` build. Do **not** downgrade One-OS to ESP-IDF v6.0.2. Do **not** switch to Linux `chip-tool`.

The direct connectedhomeip starting revision is:

- repository: `espressif/connectedhomeip`
- revision: `539342f32d5f4dc93761c2f9325afe29270068f1`
- provenance: this is the exact connectedhomeip revision previously pinned by esp-matter `release/v1.6.1`
- ESP32 integration point exists at `config/esp32/components/chip`

This revision is only the **initial direct connectedhomeip pin**. If real ESP-IDF v6.1 compiler/API evidence requires another connectedhomeip revision, a different immutable revision may be selected, but it must remain ESP32-capable, pinned, reproducible, and justified by concrete build/API evidence. Never go back to `esp-matter` merely to obtain convenience wrappers.

Current One-OS `main` already integrates and validates these eight L2 families:

- Home Assistant
- ESPHome
- Theengs
- ZHA / zigpy
- OpenThread
- Kismet
- Nmap
- Wireshark

Preserve them. Matter is the final L2 family being completed.

Physical Matter hardware is **not required** for branch completion.

Before editing code, read:

- repository root `README.md`;
- this `AGENTS.md`;
- `docs/research/matter-chip-tool-l2-api.md`;
- current `main` application docs:
  - `docs/application/nearby-devices-browser-controller.md`
  - `docs/application/nearby-devices-product-rules.md`
  - `docs/application/provisioning-web-management.md`

If `main` has moved, synchronize it into this branch first. Do not merge this research branch into `main` yourself.

## What to preserve from the current Matter candidate

Do not throw away the existing Matter work. Preserve and adapt useful One-OS-owned code, especially:

- `matter_l2.h` public API shape unless a native connectedhomeip requirement makes a small change unavoidable;
- fixed-capacity result structures and `matter_l2_bounds.*`;
- request IDs and bounded in-flight request slots;
- timeout/cancel state-machine intent;
- fixed-capacity node probe result model;
- `ReadClient::Callback` streaming probe logic;
- controller/fabric persistence concepts;
- fixed PAA trust-store concept;
- operational-credentials provider abstraction;
- subscription slot concept;
- commissioning state/callback concept;
- host bounded-memory tests and scope/security checks.

This is a **dependency and implementation-layer replacement**, not permission to redesign the One-OS application architecture.

## Remove the esp-matter dependency

The finished branch must not require the `esp-matter` framework.

Migrate away from and remove usage of:

```text
esp_matter::start(...)
esp_matter::client::interaction::read::*
esp_matter::client::interaction::write::*
esp_matter::client::interaction::invoke::*
esp_matter::client::interaction::subscribe::*
esp_matter::controller::data_model::*
#include "esp_matter.h"
#include "esp_matter_client.h"
esp_matter
esp_matter_controller
```

Replace the `third_party/esp-matter` dependency with a direct, pinned connectedhomeip dependency, preferably `third_party/connectedhomeip`, after the direct build path is established.

Update `.gitmodules`, `firmware/CMakeLists.txt`, component dependencies, CI checkout, and related configuration accordingly. Do not leave a hidden/unused esp-matter submodule after migration is complete.

The connectedhomeip ESP32 component at `config/esp32/components/chip` is the intended starting integration point. Set `CHIP_ROOT`/component paths as required by that revision rather than importing esp-matter components.

## Approved Matter ownership

Matter / CHIP owns protocol/controller operations **after the application identifies a Matter candidate**.

Owned here:

1. controller initialization/shutdown;
2. persisted single-controller/fabric state;
3. CASE path to already-authorized operational nodes;
4. bounded node interrogation;
5. Interaction Model attribute Read;
6. Write;
7. Invoke;
8. Subscribe/Unsubscribe;
9. explicit user-triggered commissioning:
   - on-network;
   - BLE + Wi-Fi;
   - BLE + Thread.

Public APIs to preserve/finish include:

```c
chip_controller_init(...);
chip_controller_shutdown(...);
chip_controller_is_ready(...);
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

Matter must not own:

- generic BLE scanning;
- generic mDNS/DNS-SD scanning;
- Matter candidate discovery/matching;
- Thread network management;
- HA Device/Entity logic;
- Device DB logic;
- a duplicate recognition database.

No Matter L2 code may call or depend on another One-OS L2 family.

## Direct connectedhomeip controller implementation

Use connectedhomeip native controller APIs directly. Prefer the upstream model already used by CHIP Tool/controller code, but build only the embedded controller capabilities One-OS needs.

### Platform / controller lifecycle

Replace `esp_matter::start()` with the connectedhomeip ESP32 platform initialization required by the pinned revision. Use native CHIP platform/controller facilities such as the appropriate combination of:

- `DeviceLayer::PlatformMgr()`;
- connectedhomeip ESP32 platform initialization;
- `DeviceControllerFactory`;
- `DeviceCommissioner`;
- `FabricTable`;
- `PersistentStorageOperationalKeystore`;
- `PersistentStorageOpCertStore`;
- `GroupDataProvider`;
- `DefaultSessionKeystore`;
- `PersistentStorageDelegate` / ESP32 persisted storage.

Do not enable a Matter device/server data model merely to satisfy controller initialization. Keep controller-only/server-interactions-off configuration where supported.

### CASE

Continue using native connectedhomeip controller session establishment, e.g. `DeviceCommissioner::GetConnectedDevice()` / the matching API in the pinned revision.

The application supplies an already-authorized operational Node ID. Matter L2 does not discover candidates.

### Read and bounded Node Probe

Use `ReadClient` directly.

`matter_node_probe()` must remain fixed-capacity and streaming:

- `ReadClient::Callback`;
- caller/slot-owned fixed arrays;
- no `BufferedReadCallback`;
- no application-owned unbounded `std::vector`/whole-list accumulation;
- Descriptor list `ReplaceAll` / `AppendItem` reports stream into bounded records;
- overflow -> partial/truncated result, never dynamic growth.

Generic single-attribute Read must decode directly from Matter TLV into bounded `chip_value_t`.

### Write

Replace the esp-matter write helper with native connectedhomeip Interaction Model write facilities, using the appropriate `WriteClient` APIs for the pinned revision.

Keep payload handling bounded. Prefer direct TLV/value encoding under One-OS control rather than introducing another general JSON framework. If the existing public JSON argument is retained temporarily for API compatibility, parsing/encoding must remain bounded and deterministic.

### Invoke

Replace the esp-matter invoke helper with native connectedhomeip command APIs, e.g. the appropriate `CommandSender` / command encode/send interfaces for the pinned revision.

Keep command payload encoding bounded and deterministic.

### Subscribe

Use `ReadClient` subscription mode directly with fixed subscription slots and bounded attribute decoding.

Preserve explicit unsubscribe through connectedhomeip Interaction Model facilities. Do not create an unbounded automatic-resubscription queue.

### Commissioning

Use native connectedhomeip controller commissioning APIs directly, including the pinned revision's equivalents of:

- `DeviceCommissioner`;
- `AutoCommissioner`;
- `RendezvousParameters`;
- `CommissioningParameters`;
- `PairDevice()` / matching controller entry point.

Support only the approved explicit workflows:

- known peer IP/port + setup PIN for on-network commissioning;
- BLE rendezvous + Wi-Fi credentials;
- BLE rendezvous + Thread operational dataset.

Do not perform generic BLE scanning or mDNS discovery inside Matter L2.

## Controller-only footprint

The direct connectedhomeip build should avoid unrelated Matter device-side/server features where the build system permits.

Prefer disabling unused capabilities such as:

- Matter server interactions/data model;
- device endpoint/cluster server framework;
- CHIP shell;
- test apps/tests;
- OTA provider/requestor if not needed by the controller;
- bridge/device-side features;
- test attestation store;
- example operational credentials issuer;
- other example-only tooling.

Keep only controller-required Crypto, SecureChannel/PASE/CASE, Fabric/Credentials, Transport, Messaging, Interaction Model client, Controller/Commissioner, BLE commissioning transport, Inet and ESP32 platform support.

Do not weaken compiler/security settings repository-wide to reduce build friction.

## Cancellation, lifetime, and sensitive data

These are completion requirements, not optional cleanup.

- Once an async request is accepted, cancellation must produce exactly one terminal `CHIP_STATUS_CANCELLED` callback.
- Timeout, cancellation, CASE failure, IM failure and success must never double-call the user callback.
- Do not reuse a request/subscription slot while a native connectedhomeip object can still callback into that slot.
- Commissioning cancellation must stop pairing and still complete the caller exactly once.
- Explicitly wipe full temporary sensitive buffers when no longer needed:
  - Wi-Fi password;
  - Thread operational dataset;
  - IPK scratch copies;
  - NOC/ICAC/RCAC scratch buffers where appropriate;
  - setup/commissioning credential scratch data where copied.
- Never log setup PINs, Wi-Fi passwords, Thread datasets, IPK, private CA material or credential bytes.
- NVS/persistent-storage initialization failures must fail closed; do not automatically erase user state to recover.

## Build hygiene

One-OS remains on:

```text
ESP-IDF v6.1
Target: esp32c6
```

Do not downgrade IDF.

Do not restore repository-wide suppressions such as:

```text
-Wno-format-security
-Wformat=0
```

If a connectedhomeip source requires a compatibility workaround, first determine whether it is an API/build-definition issue. Any unavoidable warning workaround must be scoped to the narrowest CHIP target and documented.

Do not modify or refactor HA, ESPHome, Theengs, ZHA/zigpy, OpenThread, Kismet, Nmap or Wireshark to make Matter easier.

## CI and work style

Do not stop after each compiler error and ask for permission.

Work iteratively:

```text
establish direct connectedhomeip component build
-> compile
-> fix first real error
-> compile again
-> continue through link
-> run size
-> run size-components
-> clean up dependency migration
-> final combined CI
```

Preserve the combined host-test job for the existing eight L2 families and add/retain Matter bounded/scope tests.

The final CI must validate the **combined One-OS + Matter tree**, not a Matter-only sample.

## Hardware is not a completion gate

Do not wait for or request:

- Waveshare ESP32-C6 hardware;
- serial access;
- a live Matter target;
- a pre-provisioned Fabric;
- live CASE/ACL interoperability;
- runtime heap/HWM measurements.

Unavailable runtime-only metrics must be reported exactly as `NOT_MEASURED`. They are not blockers.

## Completion gate

The branch is complete only when all software conditions below are satisfied:

- current `main` is preserved/synchronized;
- `esp-matter` is no longer a runtime/build dependency;
- direct pinned connectedhomeip ESP32 controller integration is used;
- Controller/Fabric/CASE are implemented with real native APIs;
- bounded Node Probe and Read are implemented with direct `ReadClient`;
- Write is implemented with native connectedhomeip write APIs;
- Invoke is implemented with native connectedhomeip command APIs;
- Subscribe/Unsubscribe is implemented with native connectedhomeip subscription APIs;
- approved commissioning paths use native connectedhomeip controller APIs;
- cancellation/lifetime rules are correct;
- temporary sensitive buffers are cleared;
- all existing eight-family host tests pass;
- Matter bounded/scope tests pass;
- ESP-IDF v6.1 / ESP32-C6 `idf.py build` passes;
- `idf.py size` passes;
- `idf.py size-components` passes;
- no global compiler-security weakening is introduced.

Update `docs/research/matter-chip-tool-l2-api.md` and the Matter component README to describe the final direct-connectedhomeip architecture and exact pinned revision.

Commit all work on `research/matter-chip-tool-l2-api` and report:

- final commit SHA;
- exact connectedhomeip repository/revision;
- removal status of esp-matter;
- APIs implemented;
- host/scope test results;
- ESP32-C6 build/link result;
- size and size-components result;
- runtime-only metrics as `NOT_MEASURED`;
- any truly unresolved blocker with the exact compiler/linker/API diagnostic.

Do **not** merge this branch into `main` yourself.