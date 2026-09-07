# Matter / CHIP Tool portable Level-2 API research — Phase 1

Status: **research only; no production implementation**  
Target: Waveshare ESP32-C6-Touch-LCD-1.9 / ESP-IDF / FreeRTOS / LVGL  
Branch: `research/matter-chip-tool-l2-api`  
Research date: 2026-09-07

## 1. Executive conclusion

A useful Matter Level-2 family is feasible for One-OS, but **Linux `chip-tool` itself is not the portable unit**. The portable model is the controller behavior underneath CHIP Tool:

- public commissionable-device discovery;
- a persisted controller fabric identity;
- PASE commissioning plus device attestation;
- CASE operational sessions;
- Matter Interaction Model read/write/subscribe/invoke;
- Descriptor + Basic Information reads for capability discovery.

The strongest upstream implementation boundary is `connectedhomeip`'s `DeviceController` / `DeviceCommissioner`, `CASESessionManager`, DNS-SD discovery, and Interaction Model clients. Espressif's current `esp_matter_controller` is important evidence that these primitives can be assembled as an embedded controller on ESP SoCs, but its controller example also shows that RAM/flash pressure is material and that PSRAM-oriented optimization is common. One-OS explicitly assumes **no PSRAM**, so Phase 2 must begin with an isolated build/footprint spike before any production API is accepted.

Recommended Phase-2 direction, pending approval:

1. implement a **small public discovery slice** first (`matter_*`) that does not require a fabric;
2. prove a **single-fabric embedded controller core** on ESP32-C6 with bounded resources;
3. add generic Interaction Model operations (`chip_*`);
4. add a semantic node-probe API (`matter_*`) over Descriptor + Basic Information;
5. add commissioning only after persistence, attestation and rollback behavior are proven.

Do **not** port the CHIP Tool command parser, shell, Linux storage model, Python controller, YAML runner, full generated command tree, or a self-contained Thread Border Router.

## 2. Repository constraints applied

Root `README.md` establishes these architectural constraints:

- L1 remains native ESP-IDF / FreeRTOS / LVGL / NimBLE / lwIP / IEEE 802.15.4 and thin BSP;
- no wrappers whose only purpose is renaming native APIs;
- Level-2 families are peers and must not depend on or expose types from other One-OS API families;
- higher-level APIs must follow their own upstream project's implementation model;
- the ESP32-C6 baseline assumes no PSRAM;
- current factory application partition is only `0x300000` (3 MiB), despite the 8 MiB flash device.

`AGENTS.md` additionally requires this family to remain independent of Home Assistant, OpenThread, ZHA and other One-OS L2 families, and requires public discovery to be distinguished from operations requiring ownership/commissioning credentials.

The archived beta.2 smoke source was also inspected. It validates native Wi-Fi scan, NimBLE advertisement scan and raw IEEE 802.15.4 receive on this board, but contains no previous Matter controller/commissioner implementation. It is therefore useful as evidence that the physical radio primitives work, not as an API design to preserve.

## 3. Upstream sources inspected

### connectedhomeip / CHIP Tool

Primary sources:

- `examples/chip-tool/commands/pairing/Commands.h`
  - https://github.com/project-chip/connectedhomeip/blob/master/examples/chip-tool/commands/pairing/Commands.h
- `examples/chip-tool/commands/discover/DiscoverCommissionablesCommand.cpp`
  - https://github.com/project-chip/connectedhomeip/blob/master/examples/chip-tool/commands/discover/DiscoverCommissionablesCommand.cpp
- CHIP Tool guide
  - https://project-chip.github.io/connectedhomeip-doc/development_controllers/chip-tool/chip_tool_guide.html
- `src/controller/CHIPDeviceController.h`
  - https://github.com/project-chip/connectedhomeip/blob/master/src/controller/CHIPDeviceController.h
- `src/app/InteractionModelEngine.h`
  - https://github.com/project-chip/connectedhomeip/blob/master/src/app/InteractionModelEngine.h
- `src/app/ReadClient.h`, `WriteClient.h`, `CommandSender.h`, `ClusterStateCache.h`
  - https://github.com/project-chip/connectedhomeip/tree/master/src/app
- controller data model / Descriptor cluster
  - https://github.com/project-chip/connectedhomeip/blob/master/src/controller/data_model/controller-clusters.matter
- ESP32 NimBLE Matter BLE advertising implementation
  - https://github.com/project-chip/connectedhomeip/blob/master/src/platform/ESP32/nimble/BLEManagerImpl.cpp
- host discovery notes
  - https://github.com/project-chip/connectedhomeip/blob/master/docs/tips_and_troubleshooting/discovery_from_a_host_computer.md
- Apache-2.0 license
  - https://github.com/project-chip/connectedhomeip/blob/master/LICENSE

Important observed semantics:

- `DeviceController::GetConnectedDevice()` uses `CASESessionManager::FindOrEstablishSession()` for an operational peer; already-commissioned control is therefore fabric-scoped CASE, not ordinary IP reachability.
- `DeviceCommissioner::DiscoverCommissionableNodes()` performs Matter DNS-SD commissionable discovery and accepts Matter discovery filters.
- CHIP Tool implements filters for short discriminator, long discriminator, commissioning mode, vendor ID, device type and instance name.
- pairing modes include on-network, BLE-Wi-Fi and BLE-Thread workflows.
- Interaction Model separates read, write, subscribe and invoke behavior, with subscriptions represented by `ReadClient` using Subscribe Interaction.
- Descriptor cluster provides `DeviceTypeList`, `ServerList`, `ClientList` and `PartsList`; this is the canonical basis for endpoint/capability discovery.
- Basic Information provides vendor/product/hardware/software identity after secure interaction.
- Matter BLE advertisements use the Matter service UUID `0xFFF6` and service-data identification fields; this can support bounded passive public discovery without a commissioner fabric.

### Espressif esp-matter controller evidence

Primary sources:

- controller example
  - https://github.com/espressif/esp-matter/tree/main/examples/controller
- `esp_matter_controller` component
  - https://github.com/espressif/esp-matter/tree/main/components/esp_matter_controller
- embedded controller client
  - https://github.com/espressif/esp-matter/blob/main/components/esp_matter_controller/core/esp_matter_controller_client.h
  - https://github.com/espressif/esp-matter/blob/main/components/esp_matter_controller/core/esp_matter_controller_client.cpp
- embedded read/write/subscribe/invoke commands
  - https://github.com/espressif/esp-matter/tree/main/components/esp_matter_controller/commands
- controller documentation
  - https://docs.espressif.com/projects/esp-matter/en/latest/esp32c3/controller.html
- Apache-2.0 license
  - https://github.com/espressif/esp-matter/blob/main/LICENSE

Observed implementation evidence:

- the embedded controller owns persistent operational keystore/cert store, session keystore, group data and controller fabric identity;
- commissioner setup wires a device-attestation verifier, an operational-credentials issuer, controller NOC/ICAC/RCAC, and an `AutoCommissioner`;
- read/subscribe first call `GetConnectedDevice()` (CASE) and then send Interaction Model requests;
- subscriptions expose established/terminated callbacks and bounded resubscription behavior;
- current controller examples support BLE-Wi-Fi, BLE-Thread, on-network pairing, invoke, attribute read/write/subscribe and event read/subscribe;
- the example includes a PSRAM-oriented RAM-optimization configuration, which is a warning for One-OS's no-PSRAM baseline;
- public issue history shows real interoperability/attestation edge cases with third-party Matter-over-Thread devices, so a production design must not assume the test PAA/default example path is sufficient.

## 4. Naming rule

Use **`chip_*`** when the API mirrors a controller/CHIP Tool workflow whose defining behavior is controller state, secure session establishment or an Interaction Model transaction.

Examples:

- `chip_controller_open()`
- `chip_commission_onnetwork()`
- `chip_read()`
- `chip_subscribe()`
- `chip_invoke()`

Use **`matter_*`** when the API exposes stable Matter protocol semantics rather than a CHIP Tool workflow.

Examples:

- `matter_commissionable_scan_*()`
- `matter_commissionable_t`
- `matter_node_probe()`
- `matter_node_info_t`
- `matter_endpoint_info_t`

This keeps the CHIP-derived workflow surface recognizable without naming all Matter semantics as CHIP Tool operations.

## 5. Security / ownership boundary

### Public discovery only — no ownership credential required

These operations may inspect advertisements visible to the local device and do not grant control:

- passive BLE detection of Matter commissionable advertisements (`0xFFF6` service data);
- IP DNS-SD browse of commissionable Matter nodes (`_matterc._udp`);
- discovery filters based on advertised discriminator/vendor/device-type/instance data.

Public discovery must not attempt PASE, obtain fabric credentials, or persist state.

### Ownership / commissioning credential required

Commissioning requires user-authorized commissioning material and an open commissioning window:

- setup passcode, manual code or QR payload;
- discriminator when needed for rendezvous selection;
- Wi-Fi SSID/password for BLE-Wi-Fi commissioning;
- Thread Operational Dataset for BLE-Thread commissioning;
- trusted Device Attestation verification policy/PAA roots;
- controller operational credentials/fabric identity.

Wrong passcodes, closed windows, failed attestation, invalid network credentials or fail-safe rollback must be surfaced as errors. Production code must not expose a convenient "bypass attestation" API.

### Fabric/ACL-authorized operations required after commissioning

All operational reads, writes, subscriptions, event access and invokes require a controller identity accepted by the target fabric and sufficient Access Control privilege. A node being visible by mDNS does **not** imply that One-OS may control it.

Operational discovery/resolution should therefore be treated as controller-internal/fabric-scoped behavior, not as a public "control any Matter node on LAN" API.

## 6. Candidate APIs

The signatures below are design sketches only. They intentionally use bounded C-facing types and do not expose connectedhomeip/esp-matter C++ types.

### 6.1 Public commissionable discovery

**Upstream semantics**

- CHIP Tool `discover commissionables` -> `DeviceCommissioner::DiscoverCommissionableNodes()` with DNS-SD filters.
- Matter host discovery uses `_matterc._udp` for commissionable nodes.
- Matter BLE advertising uses service UUID `0xFFF6` and Matter device-identification service data.

**Protocol prerequisite**

- BLE scan: none beyond local BLE availability.
- IP discovery: local IP stack + mDNS/DNS-SD reachability.

**Proposed API**

```c
typedef enum {
    MATTER_DISCOVERY_BLE,
    MATTER_DISCOVERY_IP,
} matter_discovery_transport_t;

typedef struct {
    bool has_long_discriminator;
    uint16_t long_discriminator;
    bool has_vendor_id;
    uint16_t vendor_id;
    bool has_product_id;
    uint16_t product_id;
    bool has_device_type;
    uint32_t device_type;
} matter_commissionable_filter_t;

typedef struct {
    matter_discovery_transport_t transport;
    uint16_t long_discriminator;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t device_type;
    uint8_t commissioning_mode;
    int8_t rssi;
    char instance_name[33];
} matter_commissionable_t;

esp_err_t matter_commissionable_scan_start(
    const matter_commissionable_filter_t *filter,
    uint32_t timeout_ms,
    matter_commissionable_cb_t cb,
    void *ctx);
esp_err_t matter_commissionable_scan_stop(void);
```

**L1 facilities**: NimBLE, lwIP/mDNS, FreeRTOS timer/task primitives.  
**Why L2**: it parses Matter-specific advertisements/DNS-SD fields, applies Matter discovery filters and emits a normalized Matter device record.  
**Footprint**: low for BLE parser; medium for DNS-SD if an additional mDNS implementation is required.  
**Credentials**: none.  
**Concurrency/radio**: public BLE scan must be mutually exclusive with Matter BLE commissioning because both need BLE host/controller ownership.  
**Provenance**: BLE parser **CLEAN-ROOM REIMPLEMENT** from protocol/upstream field semantics; DNS-SD behavior **REFERENCE-ONLY / CLEAN-ROOM REIMPLEMENT** unless connectedhomeip minimal DNS-SD is selected.  
**Disposition**: **L2 API**, highest priority.

### 6.2 Controller fabric lifecycle

**Upstream semantics**

`DeviceControllerFactory` + `DeviceController`/`DeviceCommissioner` initialize a controller with persistent storage, operational key/cert stores, fabric identity, session keystore and Interaction Model state. Espressif's embedded controller follows this same model.

**Protocol prerequisite**

Persistent fabric identity and operational credentials must exist before operational CASE control.

**Proposed API**

```c
typedef struct {
    uint64_t controller_node_id;
    uint64_t fabric_id;
    uint16_t vendor_id;
    uint16_t listen_port;
    uint8_t max_active_sessions;
    uint8_t max_subscriptions;
} chip_controller_config_t;

esp_err_t chip_controller_open(const chip_controller_config_t *cfg);
esp_err_t chip_controller_close(void);
bool chip_controller_is_ready(void);
esp_err_t chip_controller_forget_node(uint64_t node_id, uint32_t timeout_ms);
```

Do not expose `FabricTable`, `DeviceController`, CASE handles or C++ storage delegates publicly.

**L1 facilities**: NVS/key-value persistence, lwIP/UDP, entropy/crypto via ESP-IDF/mbedTLS platform integration, FreeRTOS.  
**Why L2**: owns a reusable Matter controller identity and protocol lifecycle, not merely an L1 handle.  
**Footprint**: high; connectedhomeip controller core is the main feasibility gate.  
**Credentials**: controller RCAC/NOC/private operational key/IPK are security-sensitive persistent state.  
**Concurrency**: all connectedhomeip controller operations must execute under the CHIP stack scheduling/locking rules.  
**Provenance**: controller runtime **PORT** from connectedhomeip-compatible embedded implementation; One-OS C facade **CLEAN-ROOM REIMPLEMENT**.  
**Disposition**: **L2 API**, prerequisite for all secure operations.

### 6.3 Commissioning workflows

**Upstream semantics**

CHIP Tool pairing includes:

- `pairing onnetwork`;
- `pairing ble-wifi`;
- `pairing ble-thread`;
- setup-code/QR-driven variants.

`DeviceCommissioner` performs PASE rendezvous, attestation, operational credential issuance and commissioning completion before the node becomes an operational fabric member.

**Proposed API**

```c
typedef struct {
    uint64_t node_id;
    uint32_t setup_passcode;
    uint16_t discriminator;
    uint32_t timeout_ms;
} chip_commission_base_t;

typedef struct {
    chip_commission_base_t base;
    char ssid[33];
    char password[65];
} chip_commission_wifi_t;

typedef struct {
    chip_commission_base_t base;
    const uint8_t *dataset;
    size_t dataset_len;
} chip_commission_thread_t;

esp_err_t chip_commission_onnetwork(const chip_commission_base_t *req,
                                    chip_commission_cb_t cb, void *ctx);
esp_err_t chip_commission_ble_wifi(const chip_commission_wifi_t *req,
                                   chip_commission_cb_t cb, void *ctx);
esp_err_t chip_commission_ble_thread(const chip_commission_thread_t *req,
                                     chip_commission_cb_t cb, void *ctx);
```

QR/manual setup payload parsing may later be a separate `matter_setup_payload_parse()` semantic helper rather than proliferating pairing entry points.

**L1 facilities**: BLE, IP, NVS, crypto; BLE-Thread additionally needs a valid Thread Operational Dataset and post-commissioning IPv6 reachability to that Thread network (normally via an external Border Router unless a separate application provides Thread infrastructure).  
**Why L2**: multi-step authenticated Matter workflow with rollback and persisted fabric state.  
**Footprint**: very high relative to the rest of the API; PASE, attestation, cert handling, commissioner and CASE all become live.  
**Credentials**: explicit user ownership material required.  
**Concurrency/radio**: serialize commissioning; suspend public BLE scanning; apply explicit timeout and cancellation.  
**Provenance**: **PORT** controller/commissioner behavior; no copied CHIP Tool CLI parser.  
**Disposition**: **L2 API**, but only after controller footprint and secure storage pass.

### 6.4 Generic attribute/event reads

**Upstream semantics**

`ReadClient` sends Interaction Model Read requests over an operational session. Espressif's embedded `read_command` first obtains a connected device (CASE), builds `AttributePathParams` / `EventPathParams`, then dispatches a read request and returns per-path status/data callbacks.

**Proposed API**

```c
typedef struct {
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t item_id;       /* attribute ID or event ID */
} chip_path_t;

typedef struct {
    const chip_path_t *paths;
    size_t path_count;
    uint32_t timeout_ms;
    bool fabric_filtered;
} chip_read_request_t;

esp_err_t chip_read_attributes(uint64_t node_id,
                               const chip_read_request_t *req,
                               chip_read_cb_t cb, void *ctx);
esp_err_t chip_read_events(uint64_t node_id,
                           const chip_read_request_t *req,
                           chip_event_cb_t cb, void *ctx);
```

Payloads should be returned through a bounded One-OS TLV/value view; do not expose `TLVReader *` publicly.

**L1 facilities**: controller/CASE/IP.  
**Why L2**: Matter path semantics, CASE setup and Interaction Model status mapping.  
**Footprint**: medium once controller core is resident.  
**Credentials**: commissioned fabric + target ACL privilege.  
**Provenance**: **PORT** transaction behavior, **CLEAN-ROOM REIMPLEMENT** C facade/value representation.  
**Disposition**: **L2 API**, high priority.

### 6.5 Attribute writes

**Upstream semantics**

Interaction Model `WriteClient` writes typed attribute values to one or more concrete paths and reports per-path/protocol status. Timed writes must remain explicit rather than being silently retried as non-timed writes.

**Proposed API**

```c
typedef struct {
    chip_path_t path;
    chip_value_t value;
    uint32_t timed_timeout_ms; /* 0 = non-timed */
    uint32_t timeout_ms;
} chip_write_request_t;

esp_err_t chip_write_attribute(uint64_t node_id,
                               const chip_write_request_t *req,
                               chip_write_cb_t cb, void *ctx);
```

**L1 facilities**: controller/CASE/IP.  
**Why L2**: typed Matter data-model write with Interaction Model status and optional timed interaction.  
**Footprint**: medium incremental.  
**Credentials**: commissioned fabric + write privilege.  
**Provenance**: **PORT / CLEAN-ROOM FACADE**.  
**Disposition**: **L2 API**.

### 6.6 Attribute/event subscriptions

**Upstream semantics**

CHIP Tool and esp-matter use Subscribe Interaction through `ReadClient`. A subscription has minimum/maximum reporting intervals, a subscription ID, reports, termination and optional resubscription behavior. Current Espressif code also provides explicit shutdown of one/all subscriptions.

**Proposed API**

```c
typedef uint32_t chip_subscription_id_t;

typedef struct {
    const chip_path_t *paths;
    size_t path_count;
    uint16_t min_interval_s;
    uint16_t max_interval_s;
    bool auto_resubscribe;
} chip_subscribe_request_t;

esp_err_t chip_subscribe_attributes(uint64_t node_id,
                                    const chip_subscribe_request_t *req,
                                    chip_subscription_cb_t cb, void *ctx);
esp_err_t chip_subscribe_events(uint64_t node_id,
                                const chip_subscribe_request_t *req,
                                chip_subscription_cb_t cb, void *ctx);
esp_err_t chip_subscription_cancel(chip_subscription_id_t id);
```

**L1 facilities**: controller/CASE/IP + timers.  
**Why L2**: long-lived Matter state synchronization with protocol-defined intervals and resubscription.  
**Footprint**: high incremental RAM risk because active `ReadClient` objects, paths, report buffers and session state are long-lived.  
**Credentials**: commissioned fabric + read/event privilege.  
**Concurrency**: must cap active subscriptions and path count at compile time/config time.  
**Provenance**: **PORT / CLEAN-ROOM FACADE**.  
**Disposition**: **L2 API**, after one-shot reads.

### 6.7 Command invocation

**Upstream semantics**

CHIP Tool cluster commands ultimately create an Interaction Model Invoke request. Espressif's embedded controller builds a `CommandPathParams` from endpoint/cluster/command and uses the invoke client over a connected CASE session.

**Proposed API**

```c
typedef struct {
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t command_id;
    chip_value_t fields;       /* bounded structure/TLV representation */
    uint32_t timed_timeout_ms;
    uint32_t timeout_ms;
} chip_invoke_request_t;

esp_err_t chip_invoke(uint64_t node_id,
                      const chip_invoke_request_t *req,
                      chip_invoke_cb_t cb, void *ctx);
```

The public API should be generic by numeric Matter IDs. Do not copy CHIP Tool's huge generated one-function-per-command surface into firmware.

**L1 facilities**: controller/CASE/IP.  
**Why L2**: Matter Invoke transaction, typed command payload and response/status mapping.  
**Footprint**: medium incremental if a compact value/TLV layer is used; very high if full generated controller cluster code is imported.  
**Credentials**: commissioned fabric + invoke privilege.  
**Provenance**: transaction **PORT**; generic C API/value model **CLEAN-ROOM REIMPLEMENT**.  
**Disposition**: **L2 API**, high priority.

### 6.8 Endpoint/cluster/capability probe

**Upstream semantics**

Descriptor cluster (`0x001D`) exposes:

- `DeviceTypeList` (`0x0000`);
- `ServerList` (`0x0001`);
- `ClientList` (`0x0002`);
- `PartsList` (`0x0003`).

Basic Information (`0x0028`) exposes node-level vendor/product/hardware/software identity. This is exactly the information an application needs to turn a commissioned node into a capability inventory without hardcoding CHIP Tool command names.

**Proposed API**

```c
typedef struct {
    uint16_t endpoint_id;
    uint32_t device_types[MATTER_MAX_DEVICE_TYPES_PER_ENDPOINT];
    size_t device_type_count;
    uint32_t server_clusters[MATTER_MAX_SERVER_CLUSTERS_PER_ENDPOINT];
    size_t server_cluster_count;
} matter_endpoint_info_t;

typedef struct {
    uint64_t node_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t software_version;
    char vendor_name[33];
    char product_name[33];
    matter_endpoint_info_t endpoints[MATTER_MAX_ENDPOINTS];
    size_t endpoint_count;
    bool truncated;
} matter_node_info_t;

esp_err_t matter_node_probe(uint64_t node_id, uint32_t timeout_ms,
                            matter_node_info_cb_t cb, void *ctx);
```

Unknown device types/clusters must be retained numerically, not dropped because One-OS lacks a display string.

**L1 facilities**: `chip_read_attributes()` / controller core.  
**Why L2**: this is a reusable Matter semantic workflow combining Descriptor + Basic Information into a stable node model.  
**Footprint**: low/medium incremental if implemented as bounded staged reads; avoid a whole-node unbounded cache.  
**Credentials**: commissioned fabric + read privilege.  
**Provenance**: **CLEAN-ROOM REIMPLEMENT** workflow based on Matter data model; constants may come from upstream headers at build time.  
**Disposition**: **L2 API**, highest value after generic reads.

### 6.9 Operational diagnostics

Matter diagnostics are ordinary clusters (for example General Diagnostics, Wi-Fi Network Diagnostics, Thread Network Diagnostics and Software Diagnostics). A special diagnostics L2 family is not justified initially because generic read/event operations already expose them.

**Proposed approach**: applications call `chip_read_attributes()` after checking `ServerList`; optional semantic helpers may be added only when repeated cross-application workflows emerge.

**Disposition**: **APP initially**, not a separate L2 API.

### 6.10 Open commissioning window / multi-admin

CHIP Tool exposes Administrator Commissioning workflows for opening a basic/enhanced commissioning window on a node already under authorized control. This can be useful, but it is privilege-sensitive and not needed for the minimum controller capability.

**Disposition**: **L2 API later**, after core commissioning/control is stable.

### 6.11 Group control / ICD client support

Espressif's controller contains group-data and ICD client machinery. These are valid Matter controller capabilities but increase persistent state and concurrency complexity.

**Disposition**: **DROP from first implementation tranche**; re-research only after unicast reads/subscriptions/invokes are proven on C6.

## 7. Required Matter stack footprint

A secure controller is not just a packet encoder. The minimum serious footprint includes:

- platform event loop / CHIP memory initialization;
- UDP/IPv6 networking and DNS-SD operational resolution;
- secure message/session machinery;
- PASE for commissioning;
- CASE for operational sessions;
- FabricTable and persistent operational credentials;
- operational key/certificate store;
- Interaction Model client support;
- TLV encoding/decoding;
- Device Attestation verifier + trust store for commissioning;
- BLE transport only when BLE commissioning is enabled.

Optional features to disable initially:

- Matter server interactions/advertising on the controller;
- multiple controller fabrics;
- groups/groupcast;
- ICD client support;
- OTA provider/server behavior;
- User Directed Commissioning;
- Thread Border Router;
- full generated controller cluster command model;
- interactive shell/JSON command parser;
- test/PAA bypass behavior.

## 8. ESP32-C6 RAM/flash feasibility

### Flash

One-OS has 8 MiB physical flash but the current `factory` app partition is **3 MiB**. A connectedhomeip controller build plus Wi-Fi/BLE/LVGL may exceed that partition even if total flash is available. Phase 2 must measure:

- application image size;
- largest linked components (`idf.py size-components`);
- effect of BLE commissioner on/off;
- effect of generated data-model code on/off;
- effect of subscriptions and attestation support on binary size.

Do not enlarge the partition table merely to make an untrimmed controller fit until the feature set is intentionally minimized and measured.

### RAM

This board baseline has no PSRAM. Existing One-OS already reserves RAM for FreeRTOS, Wi-Fi/NimBLE, LVGL (including a 32 KiB LVGL heap), LCD buffers and application state.

Espressif provides controller RAM-optimization configurations that move selected BSS into SPIRAM on platforms that have it. That makes a no-PSRAM C6 controller plausible only with aggressive bounds and feature trimming, not something to assume.

Phase-2 acceptance measurements must include:

- free heap before controller init;
- free/minimum heap after controller init;
- peak heap during BLE commissioning;
- peak heap during attestation/certificate handling;
- heap with 1, 2, ... subscriptions up to configured maximum;
- task stacks/high-water marks;
- Wi-Fi + BLE coexistence during commissioning;
- UI active vs UI quiesced.

### Feasibility verdict

- **Public BLE Matter discovery**: high confidence.
- **Public IP commissionable discovery**: high confidence if mDNS footprint is controlled.
- **One-shot operational read/write/invoke on a single fabric**: medium confidence, must build-measure.
- **BLE commissioning with production attestation**: medium/low confidence until measured.
- **Multiple long-lived subscriptions**: medium/low confidence without strict caps.
- **self-contained Thread Border Router on this C6**: not recommended for this API family.
- **host-size CHIP Tool feature parity**: not feasible/undesirable.

## 9. Persistence and credential requirements

Production controller persistence must include, at minimum:

- controller fabric ID and node ID;
- RCAC/NOC chain metadata;
- controller operational private key via a secure key-store abstraction;
- Identity Protection Key / group epoch key material as required by the controller stack;
- fabric index / compressed fabric ID state used for operational discovery;
- monotonic/persisted counters required by Matter secure messaging;
- commissioned-node state only where the upstream stack requires it.

Rules:

- never log setup passcodes, Wi-Fi passwords, Thread datasets, private keys or raw persisted credentials;
- clear transient commissioning credentials as soon as the workflow completes/fails;
- use NVS namespaces owned by this API family; do not share types/state with other One-OS L2 families;
- corrupted/incomplete fabric state must fail closed and offer an explicit reset/forget path;
- commissioning failure must not leave a half-added node/fabric entry;
- test credentials and attestation bypasses must not ship enabled.

A production PAA trust strategy is required. The default/test PAA path used by examples is insufficient for broad third-party interoperability.

## 10. Concurrency and radio interactions

### CHIP stack ownership

connectedhomeip controller APIs have stack-thread/locking expectations. One-OS should use a single owner context (or the upstream PlatformManager event loop) and marshal public C calls into it. Callbacks should return immutable/bounded copies/views and must not expose stack-owned C++ objects.

### BLE

Two BLE roles must not compete for host/controller ownership:

1. lightweight public Matter advertisement scan;
2. Matter BLE commissioning transport.

The API should serialize these modes and return `ESP_ERR_INVALID_STATE`/busy rather than reinitialize NimBLE under an active Matter commissioner.

### Wi-Fi / BLE / 802.15.4

ESP32-C6 shares the 2.4 GHz radio. BLE-Wi-Fi commissioning intentionally requires BLE and Wi-Fi lifecycle coordination. BLE-Thread provisioning does not justify embedding a Thread Border Router in this family; if commissioning a Thread device, use a supplied Operational Dataset and require post-commissioning IPv6 reachability through existing network infrastructure.

Any future direct Thread participation must use native/upstream platform facilities directly and remain independent of a One-OS OpenThread L2 family.

### Operation bounds

Recommended initial compile-time limits for a footprint spike (not final API promises):

- one fabric;
- one commissioning operation;
- 2 concurrent one-shot IM operations;
- 2 subscriptions;
- 8 paths per operation/subscription;
- 8 endpoints cached by `matter_node_probe`;
- 16 server clusters per endpoint;
- fixed maximum decoded value/TLV buffer.

The exact numbers must be changed based on measured C6 memory.

## 11. Interoperability and negative-test plan

### Public discovery

Positive:

- discover an uncommissioned connectedhomeip/esp-matter device over BLE;
- discover `_matterc._udp` nodes over IP;
- verify discriminator/vendor/device-type filters.

Negative:

- ignore non-Matter BLE advertisements;
- reject malformed/truncated `0xFFF6` service data;
- deduplicate repeated BLE advertisements without unbounded storage;
- handle redacted/absent VID/PID fields;
- handle mDNS records missing optional TXT fields;
- stop cleanly on timeout/cancel.

### Commissioning

Positive:

- BLE-Wi-Fi owned test device;
- on-network owned test device;
- BLE-Thread with a known Operational Dataset and reachable external Border Router.

Negative/security:

- wrong setup passcode;
- wrong discriminator / ambiguous rendezvous;
- commissioning window closed;
- invalid Wi-Fi credentials;
- invalid Thread dataset;
- untrusted/unknown PAA;
- DAC/PAI/VID/PID mismatch;
- commissioning timeout/disconnect;
- NVS write failure;
- reboot during commissioning;
- ensure no secret appears in logs;
- verify failed commissioning leaves no controllable partial state.

Do not use attestation bypass as the interoperability solution.

### Operational session / read / write / invoke

- reconnect after controller reboot using persisted fabric state;
- target reboot invalidates old CASE session and controller re-establishes it;
- unknown node ID;
- unreachable node;
- ACL denied;
- unsupported endpoint/cluster/attribute/command;
- malformed or wrong-type write/invoke value;
- timed-interaction required but omitted;
- response timeout;
- oversized response value must fail/truncate explicitly, never overflow.

### Subscriptions

- attribute subscription receives state changes;
- event subscription receives events;
- invalid min/max intervals;
- target reboot and bounded resubscribe;
- cancellation by subscription ID;
- enforce maximum active subscriptions;
- no use-after-free when callback cancels itself.

### Node probe

- simple light endpoint;
- multi-endpoint device;
- bridge/aggregator with `PartsList`;
- unknown future device type / cluster ID preserved numerically;
- server list larger than local bound sets `truncated` rather than corrupting memory.

### Cross-controller/fabric negative test

A device commissioned by another controller/fabric must **not** become controllable merely because One-OS discovers its operational service. One-OS should receive authentication/ACL/session failure unless it has been legitimately added to the relevant fabric.

## 12. Provenance plan

Both `connectedhomeip` and `esp-matter` are Apache-2.0.

| Area | Planned provenance | Rationale |
|---|---|---|
| connectedhomeip controller/session/IM runtime | **PORT** | Reusing the audited protocol implementation is safer than reimplementing Matter security/session machinery. |
| Espressif controller integration patterns | **REFERENCE-ONLY** initially; selected code may become **PORT** after footprint spike | Useful embedded proof, but One-OS should not blindly import the full example/component surface. |
| One-OS C-facing `chip_*` API | **CLEAN-ROOM REIMPLEMENT** | Stable bounded C ABI must not expose upstream C++ types. |
| Matter BLE advertisement parser | **CLEAN-ROOM REIMPLEMENT** | Tiny protocol-semantic parser avoids full stack dependency for public discovery. |
| Descriptor/Basic Information node-probe workflow | **CLEAN-ROOM REIMPLEMENT** | Compose standard Matter attributes into a reusable One-OS semantic result. |
| CHIP Tool CLI/parser/interactive shell | **REFERENCE-ONLY** | Host UX is not a portable embedded API. |
| CHIP Tool generated per-cluster command surface | **DROP** | Too large; use generic numeric path/invoke API. |
| YAML/Python test runners | **TEST/TOOL / REFERENCE-ONLY** | Useful for interoperability comparison, not firmware. |
| Matter cluster/device metadata tables | **DATA** only if a bounded subset is needed | Prefer upstream compile-time IDs and preserve unknown IDs numerically. |

Any future copied/ported source must retain Apache-2.0 notices and One-OS provenance notes.

## 13. Explicit exclusions

The following are excluded from the first production implementation:

- Linux CHIP Tool executable/CLI;
- Python CHIP Controller;
- YAML test runner in firmware;
- full generated cluster command tree;
- unbounded JSON-to-TLV command input;
- arbitrary raw TLV pointers in public API;
- multiple fabrics;
- Matter server role on the handheld controller;
- controller operational advertising unless proven necessary;
- groupcast/groups;
- ICD client/check-in support;
- OTA provider/server functionality;
- User Directed Commissioning;
- self-contained Thread Border Router;
- attestation bypass/test-PAA fallback for production;
- fabric credential extraction/import from third-party controllers;
- controlling devices not legitimately commissioned/authorized for this controller.

## 14. Phase-2 entry gates

Before production implementation, explicit user approval is required.

After approval, the first engineering action should still be a **non-production feasibility spike** that answers:

1. Can a minimal connectedhomeip controller/commissioner build link for ESP32-C6 within the current 3 MiB app partition?
2. What is minimum/peak internal RAM with Wi-Fi + BLE + LVGL active and no PSRAM?
3. Can one fabric persist across reboot and re-establish CASE to a commissioned Wi-Fi node?
4. Can one generic Descriptor read complete with bounded buffers?
5. What features must be compiled out to keep a safe heap margin?

Only after those measurements should final API limits and the direct-connectedhomeip vs selected-esp-matter port choice be frozen.

## 15. Prioritized API table

| Priority | Proposed API/capability | Naming | Credential boundary | Disposition | Footprint risk | Phase-2 recommendation |
|---|---|---|---|---|---|---|
| P0 | BLE commissionable discovery | `matter_commissionable_scan_*` | Public | **L2 API** | Low | Implement first; NimBLE + bounded `0xFFF6` parser. |
| P0 | IP commissionable discovery | `matter_commissionable_scan_*` | Public | **L2 API** | Low/Medium | Implement with bounded DNS-SD/mDNS integration. |
| P0 | Controller fabric/persistence core | `chip_controller_*` | Own controller credentials | **L2 API** | **High** | Footprint spike before production. Single fabric only. |
| P0 | Attribute read / event read | `chip_read_*` | Fabric + ACL | **L2 API** | Medium | First secure IM operation to prove CASE + TLV path. |
| P0 | Descriptor + Basic Information probe | `matter_node_probe` | Fabric + read ACL | **L2 API** | Medium | Main capability-discovery semantic API. |
| P1 | Command invoke | `chip_invoke` | Fabric + invoke ACL | **L2 API** | Medium | Generic numeric IDs; no generated CLI surface. |
| P1 | Attribute write | `chip_write_attribute` | Fabric + write ACL | **L2 API** | Medium | Typed bounded value + timed interaction support. |
| P1 | Attribute/event subscribe | `chip_subscribe_*` | Fabric + read/event ACL | **L2 API** | **High RAM** | Add after one-shot reads; hard cap subscriptions/paths. |
| P1 | On-network commissioning | `chip_commission_onnetwork` | Setup credential + attestation | **L2 API** | High | Add after persistence/attestation is production-safe. |
| P1 | BLE-Wi-Fi commissioning | `chip_commission_ble_wifi` | Setup + Wi-Fi creds + attestation | **L2 API** | **Very High** | Serialize BLE ownership; measure peak heap. |
| P2 | BLE-Thread commissioning | `chip_commission_ble_thread` | Setup + Thread dataset + attestation | **L2 API** | **Very High** | Only with external/network Thread reachability; no OTBR dependency. |
| P2 | Open commissioning window / multi-admin | `chip_open_commissioning_window` | Admin privilege | **L2 API later** | Medium | Defer until core secure control is proven. |
| P2 | Diagnostics helpers | `matter_*` semantic helpers only if repeated need appears | Fabric + ACL | **APP initially** | Low | Use generic reads first. |
| P3 | Group control / ICD | none initially | Fabric + extra persistent keys | **DROP first tranche** | High | Re-research later. |
| Excluded | Host CHIP Tool CLI/Python/YAML/full generated command tree | none | n/a | **DROP / TEST-TOOL** | Extreme | Never make this the firmware API surface. |
| Excluded | Self-contained Thread Border Router | none | network infrastructure | **DROP from family** | Extreme | Separate infrastructure concern; not this L2 API. |

## 16. Final Phase-1 recommendation

Proceed to Phase 2 **only with explicit approval**, and do not start by implementing commissioning.

The safest sequence is:

1. public BLE/IP Matter commissionable discovery;
2. minimal controller footprint spike with one persisted fabric;
3. one generic CASE-backed Descriptor read;
4. `matter_node_probe`;
5. generic invoke/write;
6. bounded subscriptions;
7. commissioning with production attestation and secure persistence.

This sequence preserves One-OS's clean L1/L2 boundary, gives early discovery value, and prevents a host-sized Matter controller from being imported before the ESP32-C6 no-PSRAM resource envelope is proven.
