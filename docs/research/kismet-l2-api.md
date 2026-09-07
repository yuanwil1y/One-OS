# Kismet Level-2 API — Phase 1 Research

Date: 2026-09-07  
Branch: `research/kismet-l2-api`  
Status: research only; no production implementation is authorized.

## 1. Scope and constraints

One-OS keeps ESP-IDF Wi-Fi/BLE/IEEE 802.15.4, FreeRTOS, and the BSP as Level 1. A Kismet-derived Level-2 API is justified only where it adds reusable tracking, relationship, scheduling, parsing, or state semantics beyond one-call wrappers.

The target is Waveshare ESP32-C6-Touch-LCD-1.9: ESP32-C6, 8 MB flash, no PSRAM assumed. The repository build currently targets ESP-IDF v6.1. ESP32-C6 has 512 KB HP SRAM and a single shared 2.4 GHz RF path for Wi-Fi, BLE, and IEEE 802.15.4, so RAM and coexistence must be treated as hard constraints.

This research intentionally excludes Kismet's server, REST/WebSocket, database logging, Web UI, remote capture-helper protocol, packet streaming, GPS stack, and alert framework unless a small reusable embedded semantic depends on them. No Kismet production code is copied or ported in Phase 1.

## 2. Upstream baseline and provenance

Research was performed against Kismet upstream commit:

- repository: <https://github.com/kismetwireless/kismet>
- commit: `e24ee9be2b56db21c16cffe72d4334bd7232dabe`
- commit date: 2026-09-01

Primary upstream sources:

- common device tracking: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/devicetracker_component.h>
- 802.11 device/client/SSID state: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_80211_components_v2.h>
- 802.11 SSID grouping: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_80211_ssidtracker.h>
- 802.11 SSID grouping behavior: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_80211_ssidtracker.cc>
- datasource/channel-hop schema: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/protobuf_definitions/datasource.proto>
- Bluetooth tracking: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_bluetooth.h>
- BTLE-specific tracking: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_btle.h>
- IEEE 802.15.4 PHY: <https://github.com/kismetwireless/kismet/blob/e24ee9be2b56db21c16cffe72d4334bd7232dabe/phy_802154.h>
- Kismet device API concepts: <https://www.kismetwireless.net/docs/api/devices/>
- Kismet memory/expiry controls: <https://www.kismetwireless.net/docs/readme/tuning/tuning/>
- Kismet channel hopping: <https://www.kismetwireless.net/docs/readme/datasources/channelhop/>

ESP-IDF references:

- Wi-Fi API/sniffer support: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/network/esp_wifi.html>
- Wi-Fi sniffer mode: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/wifi-driver/wifi-modes.html>
- ESP32-C6 RF coexistence: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/coexist.html>
- ESP32-C6 datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf>

### License conclusion

Kismet's root `LICENSE` states that, unless otherwise noted, Kismet is GPLv2; many individual files additionally carry a GPLv2-or-later header. One-OS currently has no repository-root license file. Therefore Phase 2 should **not COPY or PORT Kismet source code** without a separate licensing decision. The safe default for the candidates below is **CLEAN-ROOM REIMPLEMENT** from documented behavior/data semantics and independent protocol specifications, with Kismet retained as provenance/reference.

## 3. What is genuinely Kismet-derived

### 3.1 Common tracked-device semantics

Kismet's common device record is much more than a capture callback. It maintains a PHY-qualified device identity plus reusable historical state including:

- first seen / last seen / modification time;
- packet, TX/RX, error, data and encrypted-packet counts;
- current channel/frequency and frequency distribution;
- last/min/max signal information;
- PHY-neutral basic type classification such as AP/client/peer;
- per-datasource seen counts;
- related-device relationships.

Kismet's public device documentation also treats a device as the central tracked entity and explicitly supports relationships between devices. Kismet can expire idle devices (`tracker_device_timeout`) and evict oldest devices after a configured maximum (`tracker_max_devices`).

For One-OS, the reusable semantic is the **bounded stateful inventory**, not Kismet's dynamic tracker-element tree, HTTP views, RRDs, or server-global registry.

### 3.2 802.11 AP/client and SSID semantics

Kismet's 802.11 layer maintains distinct state for:

- SSIDs advertised in beacons;
- SSIDs returned in probe responses;
- SSIDs actively probed by client devices;
- per-client BSSID association history with first/last seen and traffic/retry counters;
- clients associated with an AP;
- last BSSID observed for a client;
- per-SSID first/last seen and encryption/capability metadata.

The separate SSID tracker groups one SSID/security identity with sets of devices that advertised, responded to, or probed for that SSID. This is a strong Kismet-specific reusable idea: applications can ask not only "which APs exist?" but also "which names are being advertised/probed, by whom, and when?"

The embedded adaptation should keep only small, clearly useful fields. It should not reproduce Kismet's EAPOL/PMKID storage, packet snapshots, large IE trees, web-facing strings, or arbitrary tracker maps.

### 3.3 Channel hopping

Kismet treats channel selection as datasource state. Its hop model includes a channel list, rate, shuffle, shuffle-skip, and offset; its documentation describes a default of 5 hops/sec and splitting channel coverage across multiple same-type radios.

One-OS has one integrated Wi-Fi radio, so multi-radio channel splitting and offset are not useful in the first implementation. The reusable embedded semantic is a **bounded observation session with a legal channel plan, dwell/rate, cancellation, and deterministic restoration**.

### 3.4 Bluetooth/BLE tracking

Kismet's Bluetooth tracker stores reusable advertisement-level metadata such as:

- device type;
- advertised service UUIDs and solicitation UUIDs;
- per-service data bytes;
- scan data;
- advertised TX power/pathloss;
- connectable state.

Its BTLE layer adds flags derived from advertisement content and the advertising PDU type. These semantics can be clean-room mapped onto native NimBLE scan reports without importing Kismet's Linux-HCI/external-capture architecture.

This overlaps with specialized BLE parser projects only at the raw/generic advertisement level. A Kismet family must remain independent: it may track generic BLE identity/service metadata, but must not call Theengs, Home Assistant, or another project API family for semantic recognition.

### 3.5 IEEE 802.15.4 is not a useful Kismet-specific L2 target

Kismet's current `kis_802154_tracked_device` is explicitly described upstream as largely a placeholder and registers no meaningful PHY-specific tracked fields. The generic Kismet device tracker can of course count sightings, but that alone is not sufficient reason to create a Kismet-flavored 802.15.4 API in One-OS.

Disposition: use native ESP-IDF IEEE 802.15.4 at L1 or a genuinely 802.15.4/Zigbee/OpenThread-derived L2 family. Do not manufacture a Kismet 802.15.4 abstraction merely for naming symmetry.

## 4. Candidate A — bounded Wi-Fi device/SSID/relationship tracker

### Upstream behavior translated

The candidate combines the useful subset of Kismet common device tracking, 802.11 per-device state, client association maps, and SSID grouping:

1. Normalize each relevant observation into a device identity and frame role.
2. Update first/last seen and signal/channel counters.
3. For beacon/probe-response frames, update an SSID record linked to the advertising/responding BSSID.
4. For probe-request frames, update an SSID record linked to the probing station.
5. For frames that reliably identify a client/BSSID relationship, update a bounded client-to-BSSID relation with evidence and first/last seen.
6. Never infer a permanent relationship from one ambiguous frame; expose evidence/age so applications can treat results as observations, not truth.

### Product value

This enables nearby-device inventory, AP/client relationship views, probe-interest views, "new/lost device" logic, signal/channel history, and UI summaries without requiring applications to build their own tracker.

### Proposed C API shape

Names are indicative, not implementation approval:

```c
typedef struct kismet_wifi_tracker kismet_wifi_tracker_t;
typedef struct kismet_wifi_session kismet_wifi_session_t;

typedef struct {
    uint16_t max_devices;
    uint16_t max_ssids;
    uint16_t max_relations;
    uint16_t max_ssid_links;
    uint32_t device_idle_ms;
    uint32_t ssid_idle_ms;
    uint32_t relation_idle_ms;
} kismet_wifi_tracker_config_t;

typedef enum {
    KISMET_WIFI_ROLE_UNKNOWN,
    KISMET_WIFI_ROLE_AP,
    KISMET_WIFI_ROLE_CLIENT,
    KISMET_WIFI_ROLE_PEER,
} kismet_wifi_role_t;

typedef struct {
    uint8_t mac[6];
    kismet_wifi_role_t role;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    int8_t rssi_last;
    int8_t rssi_min;
    int8_t rssi_max;
    uint8_t last_channel;
    uint32_t packet_count;
    uint32_t management_count;
    uint32_t data_count;
    uint16_t channel_bitmap;
    uint32_t flags;
} kismet_wifi_device_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint32_t security_flags;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint16_t advertiser_count;
    uint16_t responder_count;
    uint16_t prober_count;
    uint32_t flags;
} kismet_wifi_ssid_t;

typedef struct {
    uint8_t client[6];
    uint8_t bssid[6];
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t evidence_flags;
} kismet_wifi_relation_t;

esp_err_t kismet_wifi_tracker_create(
    const kismet_wifi_tracker_config_t *config,
    kismet_wifi_tracker_t **out_tracker);
void kismet_wifi_tracker_destroy(kismet_wifi_tracker_t *tracker);

size_t kismet_wifi_tracker_device_count(const kismet_wifi_tracker_t *tracker);
esp_err_t kismet_wifi_tracker_get_device(
    const kismet_wifi_tracker_t *tracker, size_t index,
    kismet_wifi_device_t *out_device);

size_t kismet_wifi_tracker_ssid_count(const kismet_wifi_tracker_t *tracker);
esp_err_t kismet_wifi_tracker_get_ssid(
    const kismet_wifi_tracker_t *tracker, size_t index,
    kismet_wifi_ssid_t *out_ssid);

size_t kismet_wifi_tracker_relation_count(const kismet_wifi_tracker_t *tracker);
esp_err_t kismet_wifi_tracker_get_relation(
    const kismet_wifi_tracker_t *tracker, size_t index,
    kismet_wifi_relation_t *out_relation);
```

Query APIs should initially be valid only while no session is mutating the tracker, avoiding a large snapshot/copy mechanism. A later version can add explicit snapshot objects if concurrent reads become a real requirement.

### L1 calls composed

For full passive tracking:

- `esp_wifi_init()` / `esp_wifi_set_storage(WIFI_STORAGE_RAM)` / `esp_wifi_set_mode()` / `esp_wifi_start()` when the Kismet session owns Wi-Fi;
- `esp_wifi_set_promiscuous_rx_cb()`;
- `esp_wifi_set_promiscuous_filter()`;
- `esp_wifi_set_promiscuous(true/false)`;
- `esp_wifi_set_channel()` for hopping;
- `esp_wifi_get_country()` to constrain legal 2.4 GHz channels;
- FreeRTOS queue/task primitives for deferred parsing;
- `esp_timer_get_time()` (or equivalent monotonic time) for first/last-seen timestamps.

A low-impact AP-only session may instead use `esp_wifi_scan_start()` and `esp_wifi_scan_get_ap_records()` to feed the same AP/SSID inventory. That mode adds value because it merges repeated scans into historical state; a one-shot scan wrapper alone would not qualify as L2.

### Why this is L2

The API deduplicates observations, classifies roles, tracks history, groups SSIDs, maintains relationships, enforces capacity/expiry, and reports partial state. None of those semantics exist as one native ESP-IDF call.

### State, eviction, and partial results

Recommended initial defaults, subject to measurement:

- 64 Wi-Fi devices;
- 48 SSID groups;
- 64 client/BSSID relations;
- 96 device/SSID links;
- idle expiry: 120 s devices, 120 s SSIDs, 60 s relations.

Rules:

1. Reclaim expired entries before evicting live entries.
2. When full, evict the least-recently-seen entry in that table.
3. Relation and SSID-link entries are independently bounded; evicting a device removes links that reference it.
4. Maintain monotonically increasing counters for device/SSID/relation evictions, RX-queue drops, malformed frames, and truncated frames.
5. Session results carry explicit partial flags when any bounded resource overflowed or capture frames were dropped/truncated.

Do not expose an unbounded graph or grow per-device vectors dynamically.

### RX-path design constraint

The promiscuous callback runs in the Wi-Fi receive context and must not perform heavy parsing or allocate. It should copy only bounded data into a fixed ring/queue:

- management frames: copy up to a configured fixed maximum sufficient for required IEs, and mark truncation;
- data frames: retain only the 802.11 header/metadata needed for address-role and relationship inference;
- malformed/oversize frames increment counters and are safely discarded or marked partial.

A worker task performs length-checked 802.11/IE parsing. No per-frame heap allocation.

### RAM/flash feasibility

ESP32-C6 provides 512 KB HP SRAM, but Wi-Fi, NimBLE, LVGL, FreeRTOS, and application state all consume that pool and the board baseline assumes no PSRAM. Therefore the tracker should target an incremental steady-state budget of approximately **20–32 KB for Wi-Fi tracking**, excluding the native Wi-Fi driver itself.

A plausible fixed-storage design is roughly:

- 64 compact device records: ~5–7 KB;
- 48 compact SSID records: ~3–5 KB;
- 64 relationships + 96 SSID links: ~4 KB;
- 8-entry bounded management-frame ring plus metadata: ~5 KB;
- queues/counters/session state: a few KB.

These are design estimates, not measured sizes. Phase 2 must add `sizeof` assertions/reporting and heap-watermark tests on the real ESP-IDF v6.1 build before defaults are frozen.

Flash cost should stay modest if parsing is limited to required 802.11 management fields and basic RSN/WPA capability extraction. Do not port Kismet's large dynamic field framework, server serializers, manufacturer database, or broad vendor-IE parser set.

### Tests and edge cases

Required unit/fuzz-style cases:

- truncated 802.11 headers and truncated IE lists;
- zero-length, hidden, 32-byte, duplicate, and non-UTF-8 SSIDs;
- malformed IE length extending beyond frame;
- beacon/probe-response/probe-request deduplication;
- AP/client address-role permutations (`ToDS`/`FromDS`) and WDS/4-address frames;
- broadcast/multicast addresses not becoming client relations;
- locally administered/randomized MACs treated as ordinary observed identities, not merged heuristically;
- device/SSID/relation table overflow and deterministic LRU eviction;
- expiry at exact boundary;
- queue overflow and partial-result flags;
- cancellation while a frame is queued;
- channel change failure and restoration failure.

### Provenance and disposition

- provenance: **CLEAN-ROOM REIMPLEMENT** from Kismet tracking semantics plus independent IEEE 802.11 parsing rules and ESP-IDF APIs;
- disposition: **L2 API**;
- priority: **P0 for AP/SSID inventory state; P1 for promiscuous station/probe/relationship tracking**.

## 5. Candidate B — bounded Kismet-style Wi-Fi observation session and channel plan

### Upstream behavior translated

Kismet datasources expose channel hopping with a channel list, rate, shuffle and related controls. Kismet documents a 5 hops/sec default as a Wi-Fi-oriented compromise and automatically coordinates multiple radios.

One-OS should not reproduce datasource objects or the capture-helper protocol. The portable behavior is a finite session that owns a channel plan and guarantees cleanup.

### Proposed C API shape

```c
typedef enum {
    KISMET_WIFI_SESSION_AP_SCAN,
    KISMET_WIFI_SESSION_PASSIVE_MONITOR,
} kismet_wifi_session_mode_t;

typedef struct {
    kismet_wifi_session_mode_t mode;
    uint8_t channels[14];
    uint8_t channel_count;
    uint16_t dwell_ms;
    uint32_t max_duration_ms;
    bool shuffle;
} kismet_wifi_session_config_t;

typedef struct {
    bool running;
    bool cancelled;
    bool partial;
    uint8_t current_channel;
    uint32_t channels_visited;
    uint32_t frames_seen;
    uint32_t frames_dropped;
    uint32_t malformed_frames;
    uint32_t evicted_records;
    esp_err_t terminal_error;
} kismet_wifi_session_status_t;

esp_err_t kismet_wifi_session_start(
    kismet_wifi_tracker_t *tracker,
    const kismet_wifi_session_config_t *config,
    kismet_wifi_session_t **out_session);

esp_err_t kismet_wifi_session_cancel(kismet_wifi_session_t *session);
esp_err_t kismet_wifi_session_wait(
    kismet_wifi_session_t *session, uint32_t timeout_ms);
esp_err_t kismet_wifi_session_get_status(
    const kismet_wifi_session_t *session,
    kismet_wifi_session_status_t *out_status);
void kismet_wifi_session_destroy(kismet_wifi_session_t *session);
```

`max_duration_ms` must be finite in the first implementation. Applications that need long-running observation can explicitly start successive sessions while retaining tracker state.

### Radio ownership and restoration

This is the hardest implementation boundary.

ESP-IDF documents that `esp_wifi_set_channel()` must be called after Wi-Fi start and must not be called while a STA is scanning/connecting; ESP32-C6 also shares one RF path across Wi-Fi/BLE/802.15.4. Therefore the initial passive-monitor mode should be **exclusive-owner only**:

- if the Kismet component initializes Wi-Fi, it must stop/deinit it on all normal/error/cancel paths;
- if Wi-Fi is already initialized/owned by another application function, passive-monitor start should fail with an explicit busy/invalid-state result rather than silently changing its mode/channel;
- AP-only native scan mode may later support a documented cooperative path, but must not guess ownership;
- cancellation disables promiscuous capture, stops hopping, drains or discards the bounded RX queue, and restores the state owned by the session before signaling completion.

Do not introduce a generic project-wide radio lifecycle abstraction just to make this API convenient. Composition with BLE, OpenThread, Zigbee, Home Assistant, or other L2 families belongs in the application.

### Coexistence impact

ESP32-C6 has one 2.4 GHz RF and uses time-division coexistence. Espressif classifies Wi-Fi sniffer RX + BLE activity as supported but potentially unstable, and some Wi-Fi/802.15.4 and BLE/802.15.4 scan combinations as unstable or unsupported.

Therefore:

- do not run Kismet Wi-Fi passive monitor and Kismet BLE scan concurrently in the initial family implementation;
- do not attempt to coordinate another project's BLE/802.15.4 session internally;
- return `ESP_ERR_INVALID_STATE`/busy if another Kismet session is active;
- document that the application must schedule across independent L2 families.

### Hop defaults

A 200 ms dwell (5 hops/sec) is a reasonable Kismet-derived starting default, but not a frozen requirement. Phase 2 must benchmark tune latency, packet yield, queue loss, and coexistence on ESP32-C6. Channel lists must be intersected with the current regulatory country configuration; do not blindly force channels 1–14.

### Tests

- zero/duplicate/illegal channel entries;
- dwell below/above accepted bounds;
- finite duration expiry;
- cancel during dwell and cancel during queue processing;
- failure of `esp_wifi_set_channel()`;
- failure during init/start/promiscuous enable;
- exact cleanup after every injected failure point;
- repeated sessions with no resource leakage;
- legal-channel filtering from country configuration;
- Kismet Wi-Fi/BLE mutual exclusion.

### Provenance and disposition

- provenance: **CLEAN-ROOM REIMPLEMENT** from Kismet documented channel-hop/session concepts;
- disposition: **L2 API**, as the lifecycle/hop/cancel/restore semantics are meaningful orchestration;
- priority: **P0**, because the tracker needs an explicit bounded capture lifecycle.

## 6. Candidate C — bounded BLE advertisement tracker

### Upstream behavior translated

Kismet's Bluetooth records generic device metadata and service-advertisement content while the common tracker provides first/last seen and signal history. This can map naturally to NimBLE passive discovery reports.

### Product value

Provides a stable BLE nearby inventory with generic service metadata and history even when no higher-level recognition parser is installed. Applications can later compose these results with an independent parser family, but the Kismet implementation itself remains standalone.

### Proposed C API shape

```c
typedef struct kismet_ble_tracker kismet_ble_tracker_t;
typedef struct kismet_ble_session kismet_ble_session_t;

typedef struct {
    uint16_t max_devices;
    uint8_t max_service_uuids_per_device;
    uint8_t max_service_data_entries_per_device;
    uint8_t max_service_data_bytes;
    uint32_t device_idle_ms;
} kismet_ble_tracker_config_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    int8_t rssi_last;
    int8_t rssi_min;
    int8_t rssi_max;
    int8_t advertised_tx_power;
    bool connectable;
    uint16_t seen_count;
    uint32_t flags;
} kismet_ble_device_t;

esp_err_t kismet_ble_tracker_create(
    const kismet_ble_tracker_config_t *config,
    kismet_ble_tracker_t **out_tracker);
void kismet_ble_tracker_destroy(kismet_ble_tracker_t *tracker);

esp_err_t kismet_ble_session_start(
    kismet_ble_tracker_t *tracker,
    uint32_t max_duration_ms,
    kismet_ble_session_t **out_session);
esp_err_t kismet_ble_session_cancel(kismet_ble_session_t *session);
esp_err_t kismet_ble_session_wait(
    kismet_ble_session_t *session, uint32_t timeout_ms);
```

A separate bounded accessor can expose copied service UUID/service-data summaries. Do not expose unbounded vectors or raw pointers into tracker storage.

### L1 calls composed

The existing One-OS smoke image already demonstrated the required native path:

- `nimble_port_init()` / NimBLE host task lifecycle;
- `ble_hs_util_ensure_addr()` and address-type inference;
- `ble_gap_ext_disc()` (or `ble_gap_disc()` where appropriate);
- `BLE_GAP_EVENT_EXT_DISC` / `BLE_GAP_EVENT_DISC` reports;
- `ble_gap_disc_cancel()`;
- native NimBLE advertisement-field parsing helpers where available;
- FreeRTOS synchronization primitives.

### Why this is L2

It deduplicates advertisements, tracks history and signal extrema, retains bounded generic service metadata, expires devices, and reports partial/eviction state. A thin call to `ble_gap_ext_disc()` alone would not qualify.

### State and eviction

Suggested initial default: 64 BLE devices, LRU/idle expiry, and strict per-device caps on UUID and service-data slots. Random BLE addresses are **not** merged using speculative identity heuristics; address rotation may produce multiple tracked observations. Any future identity-resolution behavior must come from native BLE privacy/security semantics or an explicitly approved higher-level capability.

Recommended incremental memory target: **under 8–12 KB** for tracker records and metadata, excluding NimBLE's own buffers/host state.

### Coexistence/cancellation

Use passive BLE scan initially. Require a finite duration and explicit cancel path. Do not start while a Kismet Wi-Fi passive-monitor session is active. On cancellation, call native discovery cancel, wait for/handle completion, and then cleanly stop only NimBLE state owned by this session.

### Tests

- legacy and extended advertising reports;
- public/random addresses;
- duplicate reports and disabled controller duplicate filtering;
- zero-length/malformed advertising data;
- max-length local name;
- repeated/duplicate UUIDs;
- service-data truncation and partial flags;
- table/per-device metadata overflow;
- timeout/cancel race;
- host sync timeout and clean teardown;
- randomized-address non-merging.

### Provenance and disposition

- provenance: **CLEAN-ROOM REIMPLEMENT** from Kismet Bluetooth tracking semantics using NimBLE APIs;
- disposition: **L2 API**;
- priority: **P2**, after Wi-Fi tracking/session semantics are proven. Generic BLE tracking is useful, but specialized BLE parsing is better handled by its own independent project family.

## 7. Candidate D — packet/device classification metadata

A small classification subset belongs inside Candidate A/C records rather than as a standalone public classifier API:

- Wi-Fi role: unknown/AP/client/peer;
- basic Wi-Fi security capability flags derived from beacon/probe-response IEs;
- BLE connectable/device-type flags that are directly observable;
- explicit evidence flags for relationships.

Manufacturer/OUI databases, arbitrary Kismet tag maps, rich vendor IE interpretation, UAV/product recognition, EAPOL/PMKID/credential metadata, and alert signatures should not be part of the first Kismet L2 API.

Provenance: **CLEAN-ROOM REIMPLEMENT** for basic classifications.  
Disposition: **internal part of L2 records**, not a separate API.

## 8. Prior One-OS/NearBy findings

The archived `beta/smoke-v0.1.0-beta.2` source validates the hardware/native building blocks but intentionally does not provide a reusable tracking layer:

- Wi-Fi: initializes native Wi-Fi in STA mode and performs one blocking active AP scan;
- BLE: initializes NimBLE and counts passive discovery reports for a fixed 3 s window;
- IEEE 802.15.4: enables promiscuous receive and steps channels 11–26 with fixed 80 ms dwell, counting frames;
- each test owns and tears down its own native radio lifecycle.

Those tests are useful evidence that Wi-Fi/BLE/802.15.4 L1 paths have worked on the board. They should **not** be revived as architecture: they contain no persistent device identity, SSID grouping, relationship tracking, bounded eviction model, or reusable session API.

No broader legacy scanner architecture is present on the current clean branch; the repository README explicitly says the previous scanner/orchestration/discovery pipeline and project-specific Level-2 APIs were intentionally not imported.

## 9. Implementation constraints for any approved Phase 2 work

1. **No Kismet code copy/port by default.** Use clean-room behavior-level reimplementation because of GPL provenance and the repository's currently unspecified license.
2. **No unbounded containers in packet-driven paths.** All device, SSID, relation, UUID, service-data, and frame queues have fixed configured capacities.
3. **No per-frame heap allocation.** Allocate bounded tracker storage at initialization; use fixed queues/rings while running.
4. **Partial results are first-class.** Dropped/truncated/evicted observations are counted and surfaced.
5. **Finite sessions.** Initial capture APIs require `max_duration_ms` and support cancellation.
6. **Deterministic radio cleanup.** Every failure/cancel path restores or tears down exactly the radio state owned by that session.
7. **No hidden cross-family orchestration.** Kismet APIs do not call or expose Theengs, Home Assistant, Bettercap, Wireshark, OpenThread, Zigpy, or other project API types.
8. **No credential/security-offense features.** Do not store EAPOL handshakes/PMKIDs for credential capture, implement deauthentication, hostile injection/replay, cracking, jamming, bypass, hijacking, exploit delivery, or persistence.
9. **Malformed input is expected.** All 802.11 IE and BLE AD structures are length-checked and fail closed.
10. **Build/measure on target.** Freeze defaults only after ESP-IDF v6.1 build, `sizeof` accounting, heap-watermark testing, high-density RF tests, and repeated cancel/error-path tests.

## 10. Prioritized API table

| Priority | Candidate | Proposed public prefix | Product value | Main L1 composition | Provenance | Disposition |
|---|---|---|---|---|---|---|
| P0 | Bounded Wi-Fi tracker: AP/device history + SSID grouping | `kismet_wifi_tracker_*` | Persistent AP/SSID inventory, first/last seen, signal/channel stats | ESP-IDF Wi-Fi scan/promiscuous APIs + FreeRTOS | CLEAN-ROOM REIMPLEMENT | L2 API |
| P0 | Bounded Wi-Fi observation session/channel plan | `kismet_wifi_session_*` | Finite capture lifecycle, hopping, cancel, cleanup, partial-result accounting | `esp_wifi_set_promiscuous*`, `esp_wifi_set_channel`, timers/tasks | CLEAN-ROOM REIMPLEMENT | L2 API |
| P1 | Station/probe/client-BSSID relationship tracking inside Wi-Fi tracker | fields/accessors under `kismet_wifi_tracker_*` | Nearby client behavior and AP/client relationships | Wi-Fi promiscuous management/data header parsing | CLEAN-ROOM REIMPLEMENT | L2 API |
| P2 | Bounded BLE advertisement tracker | `kismet_ble_tracker_*`, `kismet_ble_session_*` | Generic BLE inventory, service UUID/data history, signal stats | NimBLE GAP passive discovery + FreeRTOS | CLEAN-ROOM REIMPLEMENT | L2 API |
| P2 | Basic Kismet-style classification metadata | record fields only | AP/client/security/connectable/evidence summaries | parser output from above | CLEAN-ROOM REIMPLEMENT | internal L2 semantics |
| — | Multi-radio datasource splitting/remote capture protocol | none | Poor fit for one integrated RF/embedded device | would require server/helper architecture | REFERENCE-ONLY | DROP |
| — | Kismet IEEE 802.15.4-specific tracker | none | Upstream PHY-specific tracker is mostly placeholder | native 802.15.4 already exists | REFERENCE-ONLY | DROP |

## 11. Exclusions

- **Kismet server/Web UI/REST/WebSocket/database architecture — DROP.** Not required for reusable embedded tracking semantics.
- **Remote datasource/capture-helper protocol — DROP.** Solves distributed Linux/server capture, not the on-device ESP32-C6 problem.
- **PCAP/PCAP-NG streaming/logging — TEST/TOOL or a separate capability, not this L2.** It does not create device-tracking semantics and may overlap other packet-analysis families.
- **Full Kismet tracker-element/JSON serialization framework — DROP.** Dynamic and RAM-heavy; fixed C records are appropriate on ESP32-C6.
- **Kismet manufacturer/OUI database — APP/REFERENCE-ONLY.** Useful presentation metadata but flash/data-maintenance heavy and not necessary for the tracking core.
- **Broad vendor-IE/product/UAV recognition — APP or another independent parser family.** Do not inflate Kismet L2 into a universal recognizer.
- **EAPOL handshake/PMKID storage, credential capture/cracking — DROP.** Outside the safety boundary and unnecessary for nearby inventory.
- **Deauthentication, hostile injection/replay, jamming, security bypass, session hijacking, exploit delivery, persistence — DROP.** Explicitly prohibited.
- **Kismet-flavored IEEE 802.15.4/Zigbee/OpenThread orchestration — DROP.** Kismet upstream adds little PHY-specific state here; use the relevant native or dedicated L2 family instead.
- **Generic project-wide radio coordinator — DROP.** Independent L2 families remain peers; application code owns composition and scheduling across them.
