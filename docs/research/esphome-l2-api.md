# ESPHome Portable Level-2 API Research — Phase 1

Status: **research complete; no production implementation performed**  
Target: **Waveshare ESP32-C6-Touch-LCD-1.9 / ESP-IDF 6.1 / NimBLE**  
Research snapshot: **ESPHome 2026.8.0**, tag commit `6f8dbb6fbc1b9c108df53e5cf78d5b2316ea3af2`  
Date: 2026-09-07

## 1. Scope and decision rules

This research follows the root `README.md` and `AGENTS.md` boundaries:

- L1 remains native ESP-IDF, NimBLE, lwIP, FreeRTOS, LVGL, IEEE 802.15.4, and the thin board BSP.
- An `esphome_*` L2 API is justified only when it adds reusable behavior by composing multiple native operations or implementing a real external protocol.
- This API family must not call, wrap, depend on, or expose public types from another project API family.
- ESPHome YAML, Python code generation, component lifecycle, automation framework, entity runtime, and whole-device runtime are explicitly out of scope.
- Device-specific code is promoted to L2 only when it represents a reusable protocol family or durable interrogation/control workflow. One-device/one-characteristic wrappers remain application/plugin data.
- All operations must be bounded for the no-PSRAM ESP32-C6 target. Connection-oriented APIs need explicit timeout/cancellation and must restore native radio state they temporarily change.

The repository contains no earlier ESPHome-specific research document. Relevant prior One-OS evidence is the archived `v0.1.0-beta.2` smoke application, which successfully exercised native ESP-NimBLE passive scanning on this exact target. The clean foundation now enables NimBLE and disables Bluedroid.

## 2. Upstream findings that materially change the design

### 2.1 ESPHome now has a platform-neutral BLE model

ESPHome 2026.8 moved advertisement-based BLE consumers toward `esphome/components/ble_device_base/`. `ble_device.h` defines platform-neutral advertisement, UUID, iBeacon, address, service-data, and listener types; 2026.8 release work migrated a large group of BLE sensor platforms to this neutral layer.

This is valuable as an implementation-model reference: One-OS should separate **raw native scan ownership (L1)** from **portable advertisement normalization and protocol decoding (L2)**.

However, the upstream implementation uses C++ containers such as `std::vector` for advertisement lists and is part of ESPHome's GPLv3 runtime. It should not be copied into One-OS.

### 2.2 The reusable part of ESPHome GATT is the orchestration contract, not the ESP32 backend

`ble_device_base/ble_gatt_client.h` defines a useful platform-neutral GATT contract:

- connect/disconnect;
- service discovery;
- characteristic and descriptor read/write;
- notification registration;
- pairing;
- connection-parameter updates;
- transient service-table ownership/release;
- one outstanding GATT operation at a time;
- completion delivery through a listener/callback surface;
- explicit ATT/platform error propagation.

ESPHome's older ESP32-specific client (`esp32_ble_client/ble_client_base.h`) directly includes and uses Bluedroid `esp_gattc_*` APIs. That backend is not portable to the One-OS baseline because One-OS explicitly builds with NimBLE and `CONFIG_BT_BLUEDROID_ENABLED=n`.

Therefore the correct provenance is **REFERENCE-ONLY** for ESPHome's behavior and **CLEAN-ROOM REIMPLEMENT** on top of ESP-NimBLE.

### 2.3 Bluetooth Proxy is not an independent One-OS L2 API

ESPHome `bluetooth_proxy` adds real orchestration: bounded connection slots, connection-state handling, service-database streaming, read/write/descriptor/notify dispatch, pairing, cache handling, and retry/lost-reply behavior. But the public behavior is intentionally coupled to ESPHome Native API protobuf messages and a Home Assistant client subscription.

That makes the complete proxy an **APP / REFERENCE-ONLY** item for One-OS rather than a peer `esphome_*` L2 API. The generic connection/session semantics are worth reimplementing independently; the Home Assistant proxy transport is not.

### 2.4 ESPHome runtime licensing affects provenance

ESPHome's repository license states that its C/C++ runtime code is GPLv3, while Python and other non-runtime parts are MIT. One-OS currently has no stated decision to adopt GPLv3 runtime code.

Phase-2 default must therefore be:

- `COPY`: **none**;
- `PORT`: **none from ESPHome C/C++ runtime**;
- ESPHome runtime C/C++: **REFERENCE-ONLY**;
- production implementation: **CLEAN-ROOM REIMPLEMENT**, preferably verified against public protocol specifications and captured test vectors;
- `api.proto` can be treated separately under ESPHome's non-C++/MIT side, but generated/runtime C++ must still not be copied.

## 3. Candidate A — portable BLE advertisement normalization

**Upstream source/module**

- `esphome/components/ble_device_base/ble_device.h/.cpp`
- `esphome/components/esp32_ble_tracker/` as an ESP32 adaptation reference

**Exact reusable behavior**

Parse a raw BLE advertisement into a stable representation containing address/address type, RSSI, name, 16/32/128-bit service UUIDs, service data, manufacturer data, TX power, appearance, flags, and optional iBeacon fields. Preserve UUID/endian semantics and reject malformed AD structures safely.

**Supported scope**

Phase 2 should initially define a bounded **legacy advertisement + scan-response** format. ESPHome's neutral representation is currently oriented around legacy payload semantics even though One-OS enables BLE 5 extended advertising. Extended advertisements must never be silently truncated; return an explicit unsupported/truncated flag until a separately bounded extended-advertisement API is approved.

**Value to One-OS**

High. It gives every ESPHome-derived advertisement protocol decoder one stable input without creating a scanner framework or taking radio ownership away from native NimBLE.

**Proposed C API / data types**

Illustrative ABI, not approved implementation:

```c
typedef struct {
    uint8_t width;      /* 2, 4, or 16 */
    uint8_t value[16];  /* canonical byte order documented by API */
} esphome_ble_uuid_t;

typedef struct {
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *scan_response;
    size_t scan_response_len;
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
} esphome_ble_adv_input_t;

esp_err_t esphome_ble_adv_parse(const esphome_ble_adv_input_t *in,
                                esphome_ble_adv_t *out,
                                esphome_ble_adv_storage_t *storage);
```

`esphome_ble_adv_storage_t` should be caller-owned fixed storage for service UUIDs/data records. `out` must expose overflow/truncation flags rather than allocate.

**Native APIs composed**

None are required by the parser itself. Raw reports come from native NimBLE L1 (`ble_gap_disc` / `ble_gap_ext_disc` callbacks). This separation is intentional.

**Why this is L2**

It implements Bluetooth AD-structure parsing, normalization, UUID handling, scan-response merging, and iBeacon recognition. It is more than a native API rename.

**ESP32-C6 feasibility / memory bounds**

Excellent. Recommended initial maximum is 31-byte advertisement + 31-byte scan response, no heap, fixed record caps supplied by the caller. Name storage can be bounded to the legacy maximum. Extended reports return an explicit status until a larger format is designed.

**Authorization/security**

Passive advertisement parsing only. Treat all lengths and text as attacker-controlled input.

**Provenance**

`REFERENCE-ONLY` upstream; `CLEAN-ROOM REIMPLEMENT` against Bluetooth Assigned Numbers/Core AD formats and independent test vectors.

**Disposition**

**L2 API — Priority P0.**

**Test vectors / edge cases**

Malformed TLV lengths; zero-length element; duplicate AD types; 16/32/128-bit UUID endianness; empty/maximum name; manufacturer/service data bounds; iBeacon valid/wrong prefix/wrong size; public/random/RPA address types; advertisement + scan-response merging; unknown AD types; oversized extended report with explicit non-silent handling.

## 4. Candidate B — bounded BLE GATT interrogation session

**Upstream source/module**

- `esphome/components/ble_device_base/ble_gatt_client.h`
- `esphome/components/bluetooth_connection/`
- `esphome/components/esp32_ble_client/` as Bluedroid-specific historical behavior only
- `esphome/components/bluetooth_proxy/` for connection-slot and failure-path behavior

**Exact reusable behavior**

A bounded stateful session that can pause conflicting scanning, connect to a discovered peer with its address type, negotiate/use MTU, discover services/characteristics/descriptors, perform one GATT operation at a time, read/write values, register notifications, write CCCD where requested, pair only on caller request, cancel/timeout, disconnect, and restore prior scan state.

**Supported scope**

Generic BLE GATT client interrogation/control for authorized devices. It is not a remote Home Assistant proxy and not a permanent radio coordinator.

**Value to One-OS**

Very high. This is the reusable behavior behind many ESPHome BLE client components and directly supports capability enumeration, reads, subscriptions, and legitimate control without hundreds of characteristic-specific wrappers.

**Proposed C API / data types**

Illustrative ABI:

```c
typedef struct esphome_ble_gatt_session esphome_ble_gatt_session_t;

typedef struct {
    uint16_t max_services;
    uint16_t max_characteristics;
    uint16_t max_descriptors;
    uint32_t operation_timeout_ms;
    bool restore_scan_state;
} esphome_ble_gatt_config_t;

esp_err_t esphome_ble_gatt_open(esphome_ble_gatt_session_t *session,
                                const esphome_ble_peer_t *peer,
                                const esphome_ble_gatt_config_t *config);
esp_err_t esphome_ble_gatt_discover(esphome_ble_gatt_session_t *session,
                                    esphome_ble_gatt_db_t *db);
esp_err_t esphome_ble_gatt_read(esphome_ble_gatt_session_t *session,
                                uint16_t value_handle,
                                uint8_t *buf, size_t cap, size_t *len);
esp_err_t esphome_ble_gatt_write(esphome_ble_gatt_session_t *session,
                                 uint16_t value_handle,
                                 const uint8_t *data, size_t len,
                                 bool response);
esp_err_t esphome_ble_gatt_subscribe(esphome_ble_gatt_session_t *session,
                                     uint16_t value_handle,
                                     uint16_t cccd_handle,
                                     esphome_ble_notify_cb_t cb, void *user);
esp_err_t esphome_ble_gatt_pair(esphome_ble_gatt_session_t *session);
esp_err_t esphome_ble_gatt_cancel(esphome_ble_gatt_session_t *session);
void esphome_ble_gatt_close(esphome_ble_gatt_session_t *session);
```

The actual Phase-2 ABI should prefer caller-owned storage and explicit async completion where blocking would interfere with UI/application tasks.

**Native APIs composed**

ESP-NimBLE GAP/GATT primitives, including scan cancel/restart, connection/termination, service/characteristic/descriptor discovery, reads/writes, MTU exchange, subscription callbacks, and security initiation. FreeRTOS synchronization may be used internally, but public types remain ESPHome-family C types.

**Why this is L2**

It owns a multi-step state machine, serializes GATT operations, manages transient discovery state, handles cancellation/timeouts, and restores scanning. A single `ble_gattc_*` rename would not qualify.

**ESP32-C6 feasibility / memory bounds**

Good, but must be designed for no PSRAM. ESPHome's portable proxy wrapper explicitly optimizes each 32-bit connection wrapper to roughly tens of bytes, but its service databases can be dynamic. One-OS should instead use caller-supplied flat arrays. A reasonable initial default budget to measure in Phase 2 is one connection with caps around 24 services / 64 characteristics / 64 descriptors; the API must return `ESP_ERR_NO_MEM`/truncated metadata rather than grow unbounded. Only one GATT request may be outstanding per session.

**Authorization/security**

Connect/read/write/subscribe only to devices the user is authorized to interact with. Pairing is explicit. Never bypass security, extract credentials, downgrade authentication, or auto-accept unsafe pairing policy silently.

**Provenance**

`REFERENCE-ONLY` ESPHome runtime; `CLEAN-ROOM REIMPLEMENT` over ESP-NimBLE.

**Disposition**

**L2 API — Priority P0/P1.**

**Test vectors / edge cases**

Scan active before connect and exact restoration after close; address type missing/incorrect; connect timeout; cancel/connect race; remote disconnect at each state; empty service database; database exceeding caller caps; duplicate UUIDs; MTU 23; characteristic read ATT error; write with/without response; descriptor access; notification registration plus CCCD write; notification during teardown; pairing success/failure; lost disconnect completion with bounded safety timeout; second operation attempted while one is pending.

## 5. Candidate C — ESPHome mDNS identity discovery

**Upstream source/module**

- `esphome/components/mdns/mdns_component.cpp`
- ESPHome Native API documentation / protocol metadata

**Exact reusable behavior**

Discover `_esphomelib._tcp` services and normalize ESPHome-specific TXT records. Current upstream publishes API port plus identity/capability metadata including `version`, `config_hash`, `mac`, `board`, optional `friendly_name`, `platform`, `network`, project name/version, and Noise encryption/provisioning markers.

**Supported scope**

Local-link discovery and identification only. An `_http._tcp` fallback may be recognized only when ESPHome-specific TXT evidence is present; generic HTTP discovery is not part of this API.

**Value to One-OS**

High. It identifies reachable ESPHome nodes without credentials and tells a later application/API probe which host/port and encryption expectations apply.

**Proposed C API / data types**

```c
typedef struct esphome_mdns_discovery esphome_mdns_discovery_t;

typedef struct {
    char hostname[64];
    uint16_t port;
    char version[33];
    char mac[18];
    char platform[24];
    char board[128];
    char network[16];
    char friendly_name[121];
    char project_name[128];
    char project_version[128];
    uint32_t flags; /* Noise/provisioning/etc. */
} esphome_mdns_device_t;

esp_err_t esphome_mdns_discover_start(esphome_mdns_discovery_t *ctx,
                                      uint32_t timeout_ms,
                                      esphome_mdns_result_cb_t cb,
                                      void *user);
esp_err_t esphome_mdns_discover_cancel(esphome_mdns_discovery_t *ctx);
```

String caps are illustrative; the final ABI should follow verified upstream maxima while allowing callers to reduce result count/storage.

**Native APIs composed**

ESP-IDF mDNS browse/query primitives plus lwIP address handling and FreeRTOS scheduling.

**Why this is L2**

It knows ESPHome's service type and TXT schema, validates/normalizes identity, deduplicates results, and reports encryption expectations. It is not a generic `mdns_query_*` wrapper.

**ESP32-C6 feasibility / memory bounds**

Excellent. Use caller-provided result capacity or callback streaming. No unbounded TXT storage. Default application tests should use a small result cap (for example 8) and explicit timeout/cancel.

**Authorization/security**

No authentication is required for mDNS metadata. Treat names/TXT values/addresses as untrusted. Do not automatically connect or provision from discovery alone.

**Provenance**

`REFERENCE-ONLY` runtime behavior; `CLEAN-ROOM REIMPLEMENT` mDNS normalization.

**Disposition**

**L2 API — Priority P0/P1.**

**Test vectors / edge cases**

Normal `_esphomelib._tcp`; Noise configured; encryption supported but no key; zero-PSK provisioning marker; project fields; missing optional fields; duplicate instances; IPv4/IPv6; oversized/malformed TXT; service disappears mid-query; timeout/cancel; ESPHome-looking `_http._tcp` fallback; generic HTTP service must not match.

## 6. Candidate D — generic BTHome v2 advertisement decoder

**Upstream source/module**

- `esphome/components/bthome_mithermometer/bthome_ble.cpp`
- `esphome/components/ble_device_base/ble_aes_ccm.*`

**Exact reusable behavior**

Recognize BTHome service UUID `0xFCD2`, parse v2 advertisement-info flags (encrypted, MAC included, trigger-based, version), optionally decrypt with a caller-supplied 16-byte bindkey using AES-CCM, and iterate typed BTHome objects with strict length validation.

ESPHome's component is named for a thermometer and only publishes a subset of decoded objects. One-OS should not reproduce that wrapper; it should expose a protocol-level object iterator/result list.

**Supported scope**

BTHome v2 service-data frames. No bindkey discovery or extraction.

**Value to One-OS**

High for passive identification/state because one protocol covers many sensors and events.

**Proposed C API / data types**

```c
esp_err_t esphome_bthome_v2_decode(const uint8_t source_mac[6],
                                   const uint8_t *service_data, size_t len,
                                   const uint8_t bindkey[16], /* NULL if plaintext */
                                   esphome_bthome_item_t *items,
                                   size_t item_cap,
                                   esphome_bthome_result_t *result);
```

`result` should contain version/encrypted/trigger/MAC/packet-id flags and `item_count`; each item should retain object ID, normalized type/value, and validity metadata.

**Native APIs composed**

No radio ownership. AES-CCM can use ESP-IDF PSA/mbedTLS facilities available in the chosen IDF build or a separately reviewed internal crypto implementation; do not copy ESPHome's GPL implementation.

**Why this is L2**

It implements a real interoperable BLE application protocol, including authenticated decryption and typed object decoding.

**ESP32-C6 feasibility / memory bounds**

Excellent. Legacy frame input is tiny; keep scratch under one advertisement, 16-byte key, 13-byte nonce, 4-byte MIC, and a caller-bounded item array. No heap.

**Authorization/security**

Only decrypt with a key supplied by the authorized user/application. Never brute-force, derive, scrape, or extract bindkeys from third-party devices.

**Provenance**

`REFERENCE-ONLY` ESPHome runtime; `CLEAN-ROOM REIMPLEMENT` from BTHome public specification and independent vectors.

**Disposition**

**L2 API — Priority P1.**

**Test vectors / edge cases**

Plain v2; encrypted v2; correct/wrong key; MIC failure; missing key; wrong version; MAC included/not included; trigger frame; duplicate packet ID; all supported object widths; text explicit length; unknown object; object length beyond frame; nonascending object IDs; maximum legal frame.

## 7. Candidate E — Xiaomi/Mijia BLE (MiBeacon-family) decoder

**Upstream source/module**

- `esphome/components/xiaomi_ble/xiaomi_ble.cpp/.h`
- Xiaomi-specific BLE sensor components consuming the parser

**Exact reusable behavior**

Recognize the Xiaomi/Mijia service-data family (`0xFE95`), parse frame flags/counter/product ID/capability offsets, identify supported product variants, decode common sensor/event data points, and optionally authenticate/decrypt supported encrypted payloads with a caller-provided bindkey.

ESPHome's parser demonstrates that one decoder supports multiple Xiaomi/Qingping/MiaoMiaoce models and common values such as temperature, humidity, illuminance, motion, soil moisture, conductivity, battery, formaldehyde, and several binary/event states.

**Supported scope**

Only verified MiBeacon-family frame revisions/product IDs with tests. Product-ID mapping should be treated as protocol data, not as hundreds of public functions.

**Value to One-OS**

High identification/state value across a real vendor ecosystem.

**Proposed C API / data types**

```c
esp_err_t esphome_xiaomi_mibeacon_decode(const uint8_t source_mac[6],
                                         const uint8_t *service_data, size_t len,
                                         const uint8_t bindkey[16],
                                         esphome_xiaomi_result_t *out);
```

Use a product identifier plus validity bitmap and fixed result fields/events rather than heap-allocated entity objects.

**Native APIs composed**

No radio ownership. Optional authenticated decryption uses reviewed IDF crypto facilities.

**Why this is L2**

A nontrivial vendor protocol family provides both identification and normalized device state.

**ESP32-C6 feasibility / memory bounds**

Excellent. Upstream encrypted frame handling is bounded to small BLE payload sizes (including 19-byte and roughly 22–24-byte variants). One-OS should reject unknown lengths/revisions explicitly.

**Authorization/security**

Caller-supplied bindkeys only. No key extraction, bypass, downgrade, or brute force.

**Provenance**

`REFERENCE-ONLY` ESPHome runtime. **CLEAN-ROOM REIMPLEMENT** and independently verify product IDs/field semantics from public protocol documentation and legal captures before production.

**Disposition**

**L2 API — Priority P1/P2.**

**Test vectors / edge cases**

Representative known product IDs; unknown product; DATA flag absent; capability flag changes payload offset; duplicate counter; malformed value length; valid encrypted 19-byte and 22–24-byte variants; wrong key/MIC; signed values; random/private address interaction; frame revision not supported.

## 8. Candidate F — Ruuvi RAW advertisement decoder

**Upstream source/module**

- `esphome/components/ruuvi_ble/ruuvi_ble.cpp/.h`

**Exact reusable behavior**

Recognize Ruuvi manufacturer company ID `0x0499` and decode RAWv1 data format `0x03` and RAWv2 `0x05`. Fields include temperature, humidity, pressure, XYZ acceleration, battery voltage, and in RAWv2 TX power, movement counter, and measurement sequence number. RAWv2 sentinel values map to invalid/unknown fields.

**Supported scope**

Ruuvi RAWv1/RAWv2 only.

**Value to One-OS**

Moderate. Compact, stable, multi-device protocol with useful passive state and excellent official-vector testability.

**Proposed C API / data types**

```c
esp_err_t esphome_ruuvi_decode(const uint8_t *manufacturer_data,
                               size_t len,
                               esphome_ruuvi_result_t *out);
```

Use a validity bitmap for sentinel/unsupported fields.

**Native APIs composed**

None; pure decoder.

**Why this is L2**

It implements a documented external protocol rather than a native wrapper.

**ESP32-C6 feasibility / memory bounds**

Excellent. RAWv1/RAWv2 have exact small lengths; no heap is required.

**Authorization/security**

Passive public advertisement data only.

**Provenance**

`REFERENCE-ONLY` ESPHome; `CLEAN-ROOM REIMPLEMENT` primarily from official Ruuvi sensor-protocol documentation/test vectors.

**Disposition**

**L2 API — Priority P2.**

**Test vectors / edge cases**

Official RAWv1 and RAWv2 vectors; exact-length failure; wrong format; wrong company ID; negative temperature/acceleration; endian checks; every RAWv2 invalid sentinel; maximum/minimum encoded values.

## 9. Candidate G — bounded ESPHome Native API client probe

**Upstream source/module**

- `esphome/components/api/api.proto`
- ESPHome Native API documentation
- `esphome/components/api/` runtime only as behavior reference

**Exact reusable behavior**

A **client**, not server/emulation layer: connect to a discovered ESPHome node, perform Hello negotiation, and retrieve bounded identity/capability data (`DeviceInfo`, and for API >= 1.15 `DeviceCapabilities`). A later extension could stream entity descriptions/states to a callback rather than materializing the whole entity database.

Current API framing is protobuf-based over TCP. Modern ESPHome supports Noise encryption; password authentication messages were removed in 2026.1. The mDNS records indicate whether Noise is configured/supported.

**Supported scope**

Phase-2 candidate should start read-only: Hello + DeviceInfo + capabilities. Entity enumeration/subscription can be a separate approval. Entity commands/control should be another explicit approval because it expands authorization and compatibility surface substantially.

**Value to One-OS**

High eventual interrogation value: authoritative ESPHome version/model/project/identity and potentially entity capabilities/state.

**Proposed C API / data types**

```c
typedef struct esphome_api_client esphome_api_client_t;

typedef struct {
    const char *host;
    uint16_t port;
    const uint8_t *noise_psk; /* NULL only when peer legitimately permits it */
    uint32_t timeout_ms;
    size_t max_frame_size;
} esphome_api_probe_config_t;

esp_err_t esphome_api_probe(const esphome_api_probe_config_t *config,
                            esphome_api_device_info_t *out);
```

Future entity enumeration should be streaming/callback based with hard frame and string limits.

**Native APIs composed**

lwIP sockets, FreeRTOS timing/cancellation, IDF crypto primitives for Noise (X25519/ChaCha20-Poly1305/SHA-256 as required), and a small bounded protobuf codec.

**Why this is L2**

It implements ESPHome's interoperable application protocol, handshake, framing, compatibility negotiation, encryption, and bounded protobuf parsing.

**ESP32-C6 feasibility / memory bounds**

Feasible but materially larger than the BLE decoders/mDNS API. It introduces TCP buffering, protobuf parsing, Noise crypto state, and API-version compatibility. Use a caller-configured hard frame cap (initially around 1–2 KiB to validate) and streaming entity callbacks. Measure flash/heap before accepting Phase 2.

**Authorization/security**

If Noise/PSK is required, the key must be supplied by the authorized user/application. Do not guess keys, downgrade encryption, or implement credential capture. Do not make zero-PSK provisioning part of the first probe API; provisioning is a separate security-sensitive workflow.

**Provenance**

`api.proto` is on the MIT/non-runtime side of ESPHome's license; ESPHome C++ runtime remains `REFERENCE-ONLY`. Preferred production approach: small `CLEAN-ROOM REIMPLEMENT` client/protobuf subset with protocol tests.

**Disposition**

**L2 API — DEFER / Priority P3.**

**Test vectors / edge cases**

TCP short reads/writes; bad frame indicator; over-cap frame length; malformed varint/protobuf; Hello API major/minor compatibility; maximum documented DeviceInfo strings; API >= 1.15 capabilities; correct/wrong Noise PSK; authentication/encryption mismatch; timeout/cancel; connection closes mid-message; duplicate/unknown protobuf fields.

## 10. Candidates that should not become public ESPHome L2 APIs

### Full ESPHome Bluetooth Proxy

**Disposition: APP / REFERENCE-ONLY.** It is a Home Assistant-facing remote BLE proxy protocol coupled to ESPHome Native API messages and subscription/retry behavior. Reuse the connection-session ideas, not the proxy public surface.

### BLE presence / RSSI / scanner sensor wrappers

**Disposition: DROP.** They largely project a native scan event into an entity/state. One-OS applications can consume native scan data or the proposed normalized advertisement parser directly.

### One-characteristic BLE client sensor/switch/output wrappers

**Disposition: DROP / APP.** A separate public function for every service/characteristic adds wrapper sprawl. Use the generic bounded GATT session plus application/device descriptors.

### ESPHome YAML, Python code generation, component lifecycle, automation, scheduler, entity runtime

**Disposition: DROP.** Explicitly outside the One-OS architecture and not portable L2 behavior.

### Full ESPHome Native API server / ESPHome-node emulation

**Disposition: DROP / APP.** One-OS should not recreate the ESPHome runtime merely to look like an ESPHome node. A bounded client probe has direct discovery/interrogation value; server emulation does not.

### One-off BLE product parsers

Examples include narrowly scoped individual Airthings, Inkbird, RadonEye, ThermoPro, Mopeka, and similar components.

**Disposition: APP / DEVICE-PLUGIN DATA by default.** Promote only when investigation demonstrates a stable multi-model protocol family with reusable identification/state/control semantics and independent vectors. Do not create hundreds of `esphome_<device>_*` APIs.

### Generic mDNS, MQTT, HTTP, Wi-Fi, sockets, FreeRTOS wrappers

**Disposition: DROP.** These belong to native ESP-IDF/lwIP/FreeRTOS L1 unless an ESPHome-specific application protocol is actually being implemented.

### Provisioning/credential-transfer features

**Disposition: APP / separate future security review.** Improv-style provisioning or ESPHome Native API zero-PSK provisioning moves credentials/configuration and is not required for discovery/interrogation. It should not be smuggled into the first L2 implementation.

## 11. Cross-cutting Phase-2 implementation requirements

If implementation is approved, every accepted API should follow these rules:

1. **C public surface only** with `esphome_*` names; no public ESPHome C++ classes, STL containers, NimBLE structs, or types from another One-OS API family.
2. **Caller-owned/bounded storage** on scan and parse paths. No per-advertisement heap allocation.
3. **Explicit overflow behavior**: return count-required/truncated/error state; never silently discard data that could change identification.
4. **Explicit timeout/cancel** for scans, connections, GATT operations, mDNS, and TCP/API operations.
5. **Native-state restoration** after any temporary scan pause, connection parameter change, or other radio-state mutation.
6. **One GATT operation outstanding per session** until measurements prove a more complex scheduler is necessary.
7. **Security is opt-in and key-driven**: pairing/decryption uses caller-supplied authorization material only; no credential acquisition/bypass logic.
8. **Protocol tests before device UI integration**. Prefer public specification vectors, then legal captured frames, then malformed/fuzz cases.
9. **Provenance notes per source file** documenting the upstream behavior consulted and the independent specification/vector used to verify the reimplementation.
10. **Build continuously on ESP32-C6 / ESP-IDF 6.1**. No intermediate step may require Bluedroid.

## 12. Prioritized API table

| Priority | Candidate | Proposed public family | Value | ESP32-C6 feasibility | Provenance | Disposition |
|---|---|---|---|---|---|---|
| P0 | BLE advertisement normalization | `esphome_ble_adv_*` | Common input for identification/protocol decoders | Excellent; fixed buffers, no heap | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P0/P1 | Bounded GATT interrogation session | `esphome_ble_gatt_*` | Enumerate capabilities; read/write/notify/pair | Good; NimBLE native, strict DB caps | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P0/P1 | ESPHome mDNS identity discovery | `esphome_mdns_*` | Discover/identify ESPHome nodes and encryption expectations | Excellent; callback/caller-bounded | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P1 | BTHome v2 decoder | `esphome_bthome_*` | Broad passive sensor/event state, optional authenticated decryption | Excellent; tiny fixed frames | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P1/P2 | Xiaomi/MiBeacon-family decoder | `esphome_xiaomi_*` | Multi-model vendor identification/state | Excellent; tiny fixed frames | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P2 | Ruuvi RAW decoder | `esphome_ruuvi_*` | Stable passive telemetry protocol | Excellent; exact tiny formats | REFERENCE-ONLY + CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P3 | ESPHome Native API read-only probe | `esphome_api_*` | Authoritative device identity/capabilities; future entity interrogation | Feasible but larger TCP/Noise/protobuf footprint | MIT protocol schema + CLEAN-ROOM runtime | **L2 API, DEFER** |
| — | Full Bluetooth Proxy | none | HA remote-proxy behavior | Technically feasible but wrong boundary | REFERENCE-ONLY | **APP** |
| — | BLE presence/RSSI/sensor wrappers | none | Little beyond native event projection | Easy but redundant | n/a | **DROP** |
| — | Individual characteristic wrappers | none | Device-specific convenience only | Easy but API sprawl | n/a | **APP/DROP** |
| — | YAML/codegen/runtime/entities | none | ESPHome configuration/runtime behavior | Poor architectural fit | n/a | **DROP** |
| — | One-off product parsers | none by default | Narrow device support | Usually easy | protocol-dependent | **APP / DEVICE-PLUGIN** |
| — | Provisioning/credential transfer | none in first phase | Setup convenience | Security-sensitive | protocol-dependent | **APP / FUTURE REVIEW** |

## 13. Recommended implementation order if Phase 2 is approved later

Do **not** begin all candidates at once.

1. `esphome_ble_adv_*` first: pure parser, smallest surface, establishes bounded result conventions and protocol-test harness.
2. `esphome_mdns_*` next: independent local-network discovery/identity path with low radio complexity.
3. `esphome_bthome_*` plus one independently specified passive decoder (Ruuvi is a good reference-quality second decoder) to validate the advertisement abstraction.
4. `esphome_ble_gatt_*` after the parser conventions are stable; implement directly on NimBLE with one session and one outstanding operation, then add failure/race tests.
5. `esphome_xiaomi_*` after independent product/protocol vectors are assembled.
6. Reassess flash/heap budget before considering the deferred `esphome_api_*` Native API probe.

This order minimizes architectural lock-in and gives One-OS useful discovery/identification capability before introducing connection and crypto-heavy protocol machinery.

## 14. Source/provenance index

Pinned upstream reference unless otherwise noted: ESPHome tag `2026.8.0` (`6f8dbb6fbc1b9c108df53e5cf78d5b2316ea3af2`).

- ESPHome license: `https://github.com/esphome/esphome/blob/2026.8.0/LICENSE`
- Neutral BLE device model: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/ble_device_base/ble_device.h`
- Neutral GATT contract: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/ble_device_base/ble_gatt_client.h`
- ESP32 BLE client (Bluedroid-specific reference): `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/esp32_ble_client/ble_client_base.h`
- Bluetooth connection wrapper: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/bluetooth_connection/bluetooth_connection_hub.h`
- Bluetooth Proxy: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/bluetooth_proxy/bluetooth_proxy.cpp`
- BTHome parser: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/bthome_mithermometer/bthome_ble.cpp`
- Xiaomi BLE parser: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/xiaomi_ble/xiaomi_ble.cpp`
- Ruuvi BLE parser: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/ruuvi_ble/ruuvi_ble.cpp`
- Native API schema: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/api/api.proto`
- mDNS records: `https://github.com/esphome/esphome/blob/2026.8.0/esphome/components/mdns/mdns_component.cpp`
- ESPHome 2026.8 release notes: `https://esphome.io/changelog/2026.8.0/`
- ESPHome Native API docs: `https://developers.esphome.io/architecture/api/protocol_details/`
- One-OS target config: `firmware/sdkconfig.defaults`
- One-OS archived NimBLE validation: tag `v0.1.0-beta.2`, `firmware/main/smoke_tests.c`

## 15. Phase-1 stop condition

Research deliverable complete. **No production code, component, build-system change, or runtime API implementation is included in this phase.**

Implementation must not begin until the user explicitly approves Phase 2 and the specific API subset to implement.
