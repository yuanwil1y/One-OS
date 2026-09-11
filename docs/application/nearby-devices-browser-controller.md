# Nearby Devices Browser / Controller — End-to-End Application Development Guide

Status: **canonical application-layer workflow specification**

Implementation order: build and validate the headless runtime first, then add GUI. See [the current task list](../pre-ui-development.md). UI sections below define the final product, not an instruction to build screens before the runtime.  
Target: **Waveshare ESP32-C6-Touch-LCD-1.9 / ESP32-C6 / ESP-IDF / FreeRTOS / LVGL**  
Application: **Nearby Devices Browser / Controller**

This document defines how One-OS turns multiple independent Level-2 project API families into one complete product workflow:

```text
scan environment
→ parse protocol evidence
→ match one integrated device database
→ identify/refine device
→ create Home Assistant Device + Entity objects
→ render devices/entities in LVGL
→ select one Entity
→ dispatch to its owning protocol controller
→ receive confirmed state/event
→ update HA State
→ refresh UI
```

The goal is not to recreate a general scanner framework. The product has one application, **Nearby Devices Browser / Controller**, and the application explicitly composes independent Level-2 API families.

The repository root `README.md` remains authoritative for the platform architecture. In particular:

- ESP-IDF, FreeRTOS, LVGL, NimBLE, lwIP and native IEEE 802.15.4 facilities remain Level 1.
- Project Level-2 families are peers.
- A Level-2 family must not call, wrap, depend on, or expose another Level-2 family's public types.
- Cross-family composition happens **only in this application layer**.
- Do not introduce a generic `nearby_*` compatibility API layer between applications and the project APIs.

---

# 1. Product model

The UI and application runtime use the Home Assistant semantic model:

```text
Device
└─ Entity
   ├─ state
   ├─ attributes
   └─ supported control
```

A physical device is represented once as an HA-style Device. Everything readable or controllable is represented as an Entity.

Examples:

```text
Xiaomi BLE Thermometer
├─ sensor.temperature
├─ sensor.humidity
└─ sensor.battery
```

```text
Zigbee Smart Plug
├─ switch.outlet
├─ sensor.power
├─ sensor.energy
└─ sensor.voltage
```

```text
Matter Light
├─ light.main
├─ sensor.power
└─ sensor.energy
```

```text
ESPHome Node
├─ sensor.temperature
├─ binary_sensor.motion
├─ switch.relay
└─ light.status_led
```

The application does not expose Kismet, Wireshark, Zigbee clusters, GATT handles or Matter paths directly in the primary UI. Those are backend details attached to Entity bindings.

Unknown devices may still appear as Devices with generic read-only metadata, but writable Entities must never be created from an ambiguous fingerprint.

---

# 2. Unique capability ownership

One capability has exactly one production owner. Similar functionality discovered in another upstream project is not implemented twice.

| Capability | Unique owner | Public family |
|---|---|---|
| Wi-Fi RF observation, AP/STA/SSID/probe tracking, channel session | Kismet | `kismet_wifi_*` |
| BLE RF observation and nearby BLE tracking | Kismet | `kismet_ble_*` |
| 802.11 management-frame and IE byte parsing | Wireshark | `wireshark_wifi_*` |
| BLE advertising AD-structure parsing | Wireshark | `wireshark_ble_*` |
| BLE model-specific passive property decoding | Theengs-derived clean-room decoders | `theengs_*` |
| BLE GATT connection/discovery/read/write/notify | ESPHome-derived generic BLE client workflow | `esphome_ble_gatt_*` |
| ESPHome Native API interrogation/control | ESPHome | `esphome_api_*` |
| mDNS/Zeroconf discovery | Home Assistant-derived discovery semantics | `ha_mdns_*` |
| SSDP discovery | Home Assistant-derived discovery semantics | `ha_ssdp_*` |
| LAN host/port/service discovery | Nmap-derived bounded workflow | `nmap_*` |
| Zigbee coordinator/interview/ZCL read-write-command-reporting | zigpy-derived workflow | `zigpy_*` |
| Zigbee quirk application and capability normalization | ZHA-derived semantics | `zha_*` |
| Thread network discovery/state/topology/authorized attach | OpenThread | `openthread_*` |
| Matter controller, IM read/write/invoke/subscribe/commissioning | Matter / connectedhomeip | `chip_*`, `matter_*` |
| Device/Entity/State semantic registry | Home Assistant semantic core | `ha_core_*` |
| Hardware/protocol fingerprint matching and entity recipes | Application Device DB | `device_db_*` |
| Dynamic packet fixture generation/oracle testing | Scapy host tooling only | no firmware Runtime API |

Important consequences:

- HA does **not** own raw Wi-Fi or raw BLE scanning.
- ESPHome does **not** own BLE advertisement parsing or BTHome/Xiaomi/Ruuvi matching in Runtime.
- Theengs does **not** own BLE GATT control.
- Wireshark does **not** own Zigbee, Thread or GATT control.
- Matter does **not** duplicate BLE or mDNS scanning. It consumes evidence selected by the application and takes over only when Matter protocol work begins.
- ZHA does **not** execute Zigbee transport. zigpy owns transport; the application passes copied semantic data between them.
- The Device DB is the only Runtime fingerprint matcher across source databases.

---

# 3. Recommended source layout

The exact component names may change during integration, but the ownership should remain visible in the source tree.

```text
firmware/
├─ components/
│  ├─ board/
│  ├─ lvgl_port/
│  │
│  ├─ kismet_l2/
│  ├─ wireshark_l2/
│  ├─ ha_discovery_l2/
│  ├─ ha_core/
│  ├─ theengs_l2/
│  ├─ esphome_l2/
│  ├─ nmap_l2/
│  ├─ zigpy_l2/
│  ├─ zha_l2/
│  ├─ openthread_l2/
│  └─ matter_l2/
│
└─ main/
   ├─ app_main.c
   ├─ nearby_devices_app.c
   ├─ nearby_devices_scan.c
   ├─ nearby_devices_match.c
   ├─ nearby_devices_control.c
   ├─ nearby_devices_ui.c
   ├─ nearby_devices_events.c
   ├─ device_db.c
   ├─ device_db_storage.c
   └─ include/
```

Host-side database tooling should live outside firmware Runtime:

```text
tools/device_db/
├─ build_device_db.py
├─ validate_device_db.py
├─ source_manifests/
└─ provenance/
```

Scapy is host-only:

```text
tools/scapy_vectors/
```

The application files above are product-specific composition logic. They are not a new reusable platform framework.

---

# 4. Core runtime data ownership

Use three distinct categories of data.

## 4.1 Protocol evidence

Short-lived data produced by scans/parsers:

- Wi-Fi BSSID/SSID/channel/security/probe/relationship evidence.
- BLE address/RSSI/connectability/local name/service UUID/service data/manufacturer data.
- mDNS service type/instance/host/address/port/TXT.
- SSDP ST/NT/USN/SERVER/LOCATION/device-description evidence.
- Nmap host/port/service evidence.
- Zigbee manufacturer/model/endpoints/clusters/attributes.
- Matter VID/PID/device type/endpoints/clusters.
- ESPHome DeviceInfo/entity descriptions.

Evidence is not the final UI model.

## 4.2 Device DB profile

Immutable application data that answers:

```text
What device/profile does this evidence match?
What decoder/quirk/backend should be used?
Which entities should exist?
How should entity values be decoded?
How should writable entities be controlled?
What stable identity key may safely merge observations?
```

## 4.3 HA runtime semantic state

RAM-owned product state:

```text
HA Device registry
HA Entity registry
HA State values
```

The UI reads only this semantic layer plus small application metadata such as scan progress and online/unavailable state.

Do not place raw packet buffers, GATT database objects, Zigbee stack pointers or Matter C++ objects in HA Device/Entity records.

---

# 5. Integrated Device DB

The Device DB is intentionally simple. It is **not another Level-2 family** and is not a generic rule VM. It is application-owned read-only recognition/entity data.

## 5.1 Runtime API

Use protocol-specific typed match entry points rather than a universal `Observation` object:

```c
device_db_status_t device_db_open(device_db_t *db);
void device_db_close(device_db_t *db);

device_db_match_status_t device_db_match_wifi(
    device_db_t *db,
    const device_db_wifi_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_ble(
    device_db_t *db,
    const device_db_ble_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_mdns(
    device_db_t *db,
    const device_db_mdns_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_ssdp(
    device_db_t *db,
    const device_db_ssdp_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_lan_service(
    device_db_t *db,
    const device_db_lan_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_zigbee(
    device_db_t *db,
    const device_db_zigbee_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_matter(
    device_db_t *db,
    const device_db_matter_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_match_status_t device_db_match_esphome(
    device_db_t *db,
    const device_db_esphome_fingerprint_t *input,
    device_db_match_result_t *out);

device_db_status_t device_db_profile_get(
    device_db_t *db,
    uint32_t profile_id,
    device_db_profile_t *out);

device_db_status_t device_db_entity_recipe_get(
    device_db_t *db,
    uint32_t profile_id,
    uint16_t recipe_index,
    device_db_entity_recipe_t *out);
```

Every match must return one of at least:

```text
MATCHED
AMBIGUOUS
NOT_FOUND
TRUNCATED / DB_ERROR
```

`AMBIGUOUS` must fail closed for control. An ambiguous result may create a generic read-only Device but must not create a writable backend binding.

## 5.2 Database record model

Conceptually one profile contains:

```text
DeviceProfile
├─ profile_id
├─ vendor
├─ model
├─ display_name
├─ icon_id
│
├─ fingerprints
│  ├─ BLE
│  ├─ Wi-Fi
│  ├─ mDNS
│  ├─ SSDP
│  ├─ LAN service
│  ├─ Zigbee
│  ├─ Matter
│  └─ ESPHome
│
├─ identity rules
│  └─ explicit stable merge-key extractors
│
├─ decoder references
│  ├─ theengs_decoder_id
│  └─ zha_quirk_id
│
├─ entity recipes[]
│  ├─ HA domain/device class/unit
│  ├─ read source
│  ├─ optional write/control binding
│  └─ optional subscription/report binding
│
└─ provenance
   ├─ upstream source
   ├─ pinned revision
   ├─ license
   └─ COPY / PORT / CLEAN_ROOM / REFERENCE_ONLY
```

No recursive rules, scripts, Python, JavaScript or runtime dynamic code execution are allowed in the DB.

## 5.3 Data-source ingestion

The host generator may use these projects as **source material**, subject to provenance/license review:

- Home Assistant BLE/Zeroconf/SSDP matcher data.
- ZHA/zha-device-handlers fingerprints and declarative quirks.
- ESPHome device/protocol knowledge where legally usable.
- Theengs behavior as reference; GPL data/code must not be silently copied into a non-GPL firmware database.
- Matter VID/PID/device-type metadata.
- Zigbee manufacturer/model fingerprints.
- One-OS-owned captures and vendor protocol documents.

Runtime must not maintain separate HA matcher DB + Theengs DB + ESPHome DB + ZHA DB and sequentially search all of them. The generator normalizes approved source data into **one indexed database**.

## 5.4 Provenance gate

Every generated record must carry source provenance in the source manifest. Records sourced only from `REFERENCE_ONLY` material require an independent specification, owned capture or clean-room derivation before shipping.

The database builder must reject records with missing provenance.

## 5.5 Storage

Recommended full-corpus deployment is a compact read-only indexed binary file on SD, because the board has no PSRAM and the corpus may grow significantly:

```text
/nearby/db/devices.nbdb
```

Runtime lookup must be bounded and read records on demand rather than loading the full database into RAM.

Small compiled fixtures may be used only in test builds. Production firmware must not embed a recognition corpus as fallback: when SD/DB is unavailable, keep generic unknown Devices and skip recognition. The SD database is the sole production corpus, as required by nearby-devices-product-rules.md.

---

# 6. Identity and deduplication rules

Do not merge two observations merely because they “look similar.” A wrong merge can cause an Entity control action to target the wrong physical device.

The Device DB may define explicit stable identity extractors such as:

- Zigbee IEEE EUI-64.
- Matter operational node identity scoped to the local fabric.
- stable protocol UUID/device ID published by a vendor protocol.
- ESPHome node identity explicitly reported by the Native API.
- stable mDNS/TXT identifier when protocol documentation guarantees its meaning.
- public BLE address only when the device uses a stable public/static identity and the profile says it is safe.

Do **not** automatically use these as cross-protocol physical identity without an explicit profile rule:

- BLE random/resolvable/random-static address.
- IP address.
- RSSI.
- SSID.
- Wi-Fi BSSID from an AP that may represent a radio/interface instead of the complete physical product.
- Matter VID/PID by themselves.
- model name by itself.

When no safe merge key exists, keep separate Device candidates rather than guessing.

---

# 7. Application task ownership

Keep tasking simple.

## 7.1 UI / HA owner task

One task owns:

- `ha_core` mutations and enumeration.
- LVGL object creation/update/destruction.
- application Device/Entity binding table.
- user interaction state.

No radio/protocol callback directly mutates LVGL or the HA registry.

## 7.2 Scan/control worker

One application worker owns high-level product operations:

- full environment scan sequence.
- Device DB lookup.
- safe active interrogation.
- user-requested control dispatch.

Individual L2 components may own internal tasks required by their upstream stack, but callbacks copy bounded results into the application's queue.

## 7.3 Bounded event queue

Use one or a small number of fixed FreeRTOS queues for application events. Example event classes:

```text
SCAN_STAGE
DEVICE_UPSERT
ENTITY_UPSERT
STATE_UPDATE
DEVICE_UNAVAILABLE
CONTROL_PENDING
CONTROL_CONFIRMED
CONTROL_FAILED
SCAN_FINISHED
```

This queue is an implementation detail of Nearby Devices, not a general One-OS event framework.

---

# 8. Full environment scan state machine

A full scan is a finite application workflow. The application owns ordering because the ESP32-C6 has one shared 2.4 GHz RF path and the Level-2 families must not coordinate each other.

Recommended order:

```text
0. Begin scan generation
1. Wi-Fi RF scan/tracking
2. BLE RF scan/tracking
3. Optional Thread network discovery
4. Optional Zigbee local-network inventory/interview
5. Restore/ensure Wi-Fi STA connectivity
6. HA mDNS discovery
7. HA SSDP discovery
8. Nmap LAN host/port/service discovery
9. Match/refine all accumulated evidence in Device DB
10. Run bounded safe-read enrichment queue
11. Materialize/update HA Device + Entity + State
12. Sweep stale ephemeral devices
13. End scan generation
```

The exact order may be tuned after hardware coexistence measurements, but the application must preserve these invariants:

- Kismet Wi-Fi passive-monitor session and Kismet BLE scan are serialized initially.
- BLE scan and Thread scan are serialized.
- Zigbee/OpenThread ownership of IEEE 802.15.4 is serialized unless a validated ESP-IDF configuration proves otherwise.
- Matter BLE commissioning never runs during a Kismet BLE scan.
- IP discovery starts only after Wi-Fi STA connectivity is valid.
- Every stage has a finite timeout and cancellation path.
- A scan does not automatically perform writes, pairing, commissioning or destructive changes.

---

# 9. Scan generation lifecycle

At the start of `Scan`:

1. increment `scan_generation`;
2. mark all ephemeral currently displayed devices unseen for this generation;
3. clear per-scan evidence/dedup tables;
4. keep protocol-authorized persistent identities (for example commissioned Matter/Zigbee nodes) as known but potentially unavailable;
5. start stage 1.

As evidence arrives, mark/create the device as seen in the current generation.

At scan completion:

- ephemeral devices not seen in the new generation are removed or marked stale according to product policy;
- authorized/commissioned devices that were not reachable remain in the Device list with `unavailable` state instead of losing controller identity;
- partial-stage failures must be shown as partial scan status rather than pretending the environment was exhaustively scanned.

No UI progress may be based only on elapsed time. Progress advances on stage completion.

---

# 10. Wi-Fi workflow

## 10.1 Scan owner: Kismet

Application calls the bounded Kismet Wi-Fi session API, for example:

```c
kismet_wifi_tracker_create(...);
kismet_wifi_session_start(...);
kismet_wifi_session_cancel(...);
kismet_wifi_session_wait(...);
```

The Kismet family owns:

- channel plan/hopping for this session;
- finite duration/cancel;
- AP/client/probe observation lifetime;
- first/last seen and RSSI/channel history;
- AP/STA/SSID/probe relationships;
- bounded tables and eviction accounting;
- cleanup of radio state that the Kismet session itself changed.

## 10.2 Byte parsing owner: Wireshark

For each captured 802.11 management frame requiring semantic parsing, the application passes bytes to:

```c
wireshark_wifi_mgmt_parse(...);
```

The Wireshark parser extracts a bounded subset such as:

- subtype.
- source/destination/BSSID.
- SSID/hidden state.
- channel.
- capability bits.
- RSN/AKM/cipher information.
- selected HT/HE information.
- bounded vendor IE descriptors.

Kismet and Wireshark must not include each other's headers. The application copies required fields between their public records.

## 10.3 Device DB matching

Application converts stable parsed/tracked fields into `device_db_wifi_fingerprint_t` and calls:

```c
device_db_match_wifi(...);
```

A match may produce a router/AP/device profile. Unmatched APs can still become generic read-only Devices showing SSID/security/channel/signal if product UX wants them.

Wi-Fi evidence by itself must not create writable controls unless the matched Device DB profile provides an independently supported controller binding.

---

# 11. BLE workflow

BLE has four deliberately separate steps.

## 11.1 RF scanning/tracking — Kismet

Application starts:

```c
kismet_ble_tracker_create(...);
kismet_ble_session_start(...);
```

Kismet owns only generic nearby inventory:

- address/address type.
- RSSI.
- connectable indication.
- first/last seen.
- seen count.
- bounded expiry.

It does not own model-specific decoding.

## 11.2 AD parsing — Wireshark

Application passes advertisement payload bytes to:

```c
wireshark_ble_adv_parse(...);
```

This yields bounded protocol fields:

- flags.
- local name.
- service UUIDs.
- service data.
- manufacturer data.
- TX power.
- appearance.
- truncation/malformed status.

## 11.3 Device recognition — Device DB

Application builds `device_db_ble_fingerprint_t` from Kismet + Wireshark evidence and calls:

```c
device_db_match_ble(...);
```

If `MATCHED`, result may contain:

```text
profile_id
theengs_decoder_id
BLE GATT backend recipe IDs
identity extraction rule
entity recipe range
```

If `AMBIGUOUS`, keep a read-only generic device until more evidence resolves it.

## 11.4 Passive value decode — Theengs

The Device DB is the matcher. Theengs must not run a second full model search.

The preferred deduplicated runtime contract is a selected-decoder form such as:

```c
theengs_decode_by_id(theengs_decoder_id, parsed_adv, out_values);
```

or an equivalent API where the application supplies the already selected model/decoder.

Theengs returns typed properties such as:

```text
temperature
humidity
battery
illuminance
moisture
button event
opening state
```

Application maps these values through Device DB entity recipes into HA State updates.

## 11.5 BLE GATT enrichment/control — ESPHome

Only if the matched profile says active GATT interrogation is useful and safe, the application may later use:

```c
esphome_ble_gatt_connect(...);
esphome_ble_gatt_discover(...);
esphome_ble_gatt_read(...);
esphome_ble_gatt_write(...);
esphome_ble_gatt_subscribe(...);
esphome_ble_gatt_disconnect(...);
```

A full environment scan may automatically perform only Device DB operations classified as **safe read-only interrogation**. It must not automatically write characteristics, pair with arbitrary devices or guess credentials.

Pairing/bonding/PIN/passkey/user approval remains explicit.

---

# 12. Home Assistant mDNS/Zeroconf workflow

After Wi-Fi STA is connected, run:

```c
ha_mdns_discover_once(...);
```

The HA discovery family owns:

- finite DNS-SD/mDNS browse/query lifecycle;
- service correlation and normalization;
- service type, instance, host, addresses, port and TXT handling;
- bounded cache/results for one discovery operation.

It does not perform product matching in Runtime.

For each service result:

```c
device_db_match_mdns(...);
```

The Device DB source data may include approved Home Assistant Zeroconf/HomeKit/Matter/ESPHome matcher knowledge.

Possible outcomes include:

- generic mDNS device.
- HomeKit service identity.
- ESPHome node candidate.
- Matter commissionable/operational service candidate.
- vendor-specific local service.

The application then schedules the appropriate **refinement backend**, but `ha_mdns_*` never calls ESPHome or Matter directly.

---

# 13. SSDP workflow

Run a bounded HA-derived discovery operation:

```c
ha_ssdp_discover_once(...);
```

Collect normalized evidence such as:

- ST / NT.
- USN.
- SERVER.
- LOCATION.
- device type.
- manufacturer/model description if safe bounded enrichment is enabled.

Then call:

```c
device_db_match_ssdp(...);
```

Typical discovered classes include televisions, media renderers, routers, NAS devices, printers and smart-home bridges.

SSDP discovery does not imply a generic UPnP control implementation. Writable Entity recipes are created only if a supported controller backend exists.

---

# 14. Nmap LAN workflow

Nmap-derived L2 is the active LAN evidence owner for devices that do not advertise enough information through mDNS/SSDP.

Recommended P0 application flow:

```c
nmap_discovery_start(...);
→ host results

nmap_port_scan_start(...);
→ open/refused/timeout evidence

nmap_service_scan_start(...);
→ bounded service evidence
```

The application uses explicit limits for:

- target count/range.
- concurrency.
- deadline.
- retries.
- port profile size.
- capture bytes per service probe.

Then:

```c
device_db_match_lan_service(...);
```

Use service evidence to refine an existing IP/mDNS/SSDP Device when a safe identity/merge rule exists.

Examples of useful service evidence:

```text
HTTP/HTTPS management
RTSP
printer service
MQTT
ESPHome API port
vendor device APIs
media protocols
```

Nmap evidence is observation, not proof of product identity. Conventional port numbers alone are insufficient for a writable device match.

---

# 15. ESPHome Native API workflow

An mDNS/Device DB match may identify an ESPHome node and supply its host/port plus expected encryption metadata.

Application may then perform a bounded read-only probe:

```c
esphome_api_probe(...);
```

After the ESPHome API implementation is expanded, the application may use equivalents of:

```c
esphome_api_entities(...);
esphome_api_subscribe(...);
esphome_api_command(...);
```

The Native API can return authoritative:

- device/node identity.
- version/model/project information.
- entity descriptions.
- current states.
- capabilities.

Application feeds stable identity back through:

```c
device_db_match_esphome(...);
```

or uses the existing profile to refine identity.

ESPHome entity descriptions map naturally to HA Device/Entity records. The Device DB may override display metadata or bindings for known hardware, but it must not duplicate the Native API transport implementation.

Noise/PSK credentials are caller/application supplied. Never downgrade or guess credentials.

---

# 16. Zigbee workflow

Zigbee differs from BLE/Wi-Fi discovery: useful device identity/capability normally requires participation in an authorized Zigbee network.

A normal full environment scan should enumerate/interview devices already belonging to the One-OS Zigbee network. It must **not** silently open permit-join or add devices.

Adding a nearby Zigbee device is a separate explicit user action.

## 16.1 Transport/interview owner — zigpy

Use zigpy-derived workflows such as:

```c
zigpy_interview_begin(...);
zigpy_interview_cancel(...);
zigpy_interview_get_snapshot(...);

zigpy_attr_read_async(...);
zigpy_attr_write_async(...);
zigpy_command_invoke_async(...);
zigpy_reporting_configure_async(...);
```

Interview snapshot should contain bounded copies of:

- IEEE address.
- manufacturer/model.
- endpoints.
- input/output clusters.
- basic device metadata.

## 16.2 Recognition owner — Device DB

The host DB generator ingests approved ZHA/zha-device-handlers fingerprint data.

Runtime application calls:

```c
device_db_match_zigbee(...);
```

A successful result may return:

```text
profile_id
zha_quirk_id
entity recipes
identity rule
```

This avoids a second independent ZHA fingerprint database scan in Runtime.

## 16.3 Quirk/capability owner — ZHA

After Device DB selected a quirk/profile, the application invokes ZHA using the selected ID, for example:

```c
zha_quirk_apply_by_id(...);
zha_capability_enumerate(...);
zha_transform_decode(...);
zha_transform_encode(...);
zha_action_decode(...);
```

Exact final function names may follow the implemented ZHA component, but the ownership rule is fixed:

- Device DB selects the profile/quirk.
- ZHA applies/normalizes it.
- zigpy performs Zigbee transactions.
- Application copies fields between ZHA and zigpy types.

## 16.4 Entity creation

ZHA normalized capabilities become HA Entities.

Examples:

```text
OnOff cluster + device semantics
→ switch / light

Temperature Measurement
→ sensor.temperature

Electrical Measurement
→ sensor.voltage / sensor.current / sensor.power

remote command + quirk mapping
→ event/action entity semantics
```

## 16.5 Explicit add-device workflow

User action:

```text
Add Zigbee Device
→ zigpy_commissioning_start(finite_window)
→ device joins
→ zigpy interview
→ Device DB match
→ ZHA quirk/capability
→ HA Device/Entities
→ commissioning window automatically closes
```

No permanent permit-join and no network-key extraction.

---

# 17. Thread workflow

OpenThread owns Thread network operations, not Matter device semantics.

Full environment scan may call:

```c
openthread_discover_networks(...);
openthread_get_state_snapshot(...);
```

These results primarily represent network infrastructure/prerequisites. A Thread network itself does not automatically become a user-controlled HA Device.

Explicit authorized operations may later use:

```c
openthread_get_local_topology(...);
openthread_attach_dataset(...);
openthread_joiner_join(...);
```

Thread supplies IPv6 connectivity. Matter-over-Thread device semantics still belong to Matter:

```text
OpenThread connectivity
→ Matter operational discovery/session
→ Matter node probe
→ HA Device/Entity
```

The application must serialize Thread discovery and BLE scanning and must serialize Thread/Zigbee native IEEE 802.15.4 ownership unless target testing proves a safe supported coexistence mode.

---

# 18. Matter workflow

Matter does not duplicate environment scanning.

Initial Matter candidates arrive through:

- Kismet BLE + Wireshark BLE parser + Device DB for commissionable BLE advertisements.
- HA mDNS + Device DB for commissionable/operational DNS-SD services.

Once the application identifies a Matter candidate, Matter becomes the owner of protocol operations.

## 18.1 Authorized existing node

For a node already on the local Matter fabric:

```text
match operational service
→ chip controller session
→ CASE
→ matter_node_probe()
→ Descriptor + Basic Information
→ Device DB refinement
→ HA Device + Entities
```

Use bounded operations such as:

```c
chip_read_attribute(...);
chip_write_attribute(...);
chip_invoke(...);
chip_subscribe_*(...);
matter_node_probe(...);
```

## 18.2 Uncommissioned node

A full environment scan may show an uncommissioned Matter Device candidate, but it must not commission automatically.

Explicit user flow:

```text
select uncommissioned Matter Device
→ user chooses Commission
→ setup code / credential input
→ attestation validation
→ chip_commission_*
→ persist legitimate fabric credentials
→ node probe
→ Device DB refinement
→ HA Device/Entities become controllable
```

Matter controller/fabric state is protocol state and may require persistent storage. HA Device/Entity runtime state remains reconstructable.

Do not expose attestation bypass as a production solution.

---

# 19. Safe active enrichment queue

After broad discovery, the application may perform additional **read-only** interrogation to turn weak evidence into useful Devices/Entities.

Possible queue items:

- ESPHome Native API `DeviceInfo` probe.
- safe BLE GATT service discovery/read defined by a matched profile.
- Zigbee interview/reinterview of already authorized network members.
- Matter `node_probe` for already authorized fabric nodes.
- bounded Nmap service probes.

Each Device DB profile marks operations with a policy such as:

```text
PASSIVE_ONLY
SAFE_READ
AUTH_REQUIRED
USER_ACTION_REQUIRED
WRITE_CONTROL
```

The full environment scan automatically executes only `PASSIVE_ONLY` and approved `SAFE_READ` operations.

It never automatically:

- writes device state.
- opens Zigbee permit-join.
- commissions Matter.
- pairs/bonds with arbitrary BLE devices.
- guesses passwords/passkeys/keys.
- changes network configuration.

---

# 20. Device materialization into HA semantics

Once a profile and sufficient identity are available, application code materializes the semantic model.

Conceptual flow:

```c
profile = device_db_profile_get(profile_id);

device_id = app_resolve_device_identity(profile, evidence);

ha_core_device_upsert(...);

for each entity recipe:
    ha_core_entity_upsert(...);
    app_entity_binding_store(entity_id, recipe.binding);

for each known initial value:
    ha_core_state_set(...);
```

The exact `ha_core_*` function names should follow the integrated HA semantic component, but it must support at minimum:

- Device upsert/get/remove/enumeration.
- Entity upsert/get/remove/enumeration by Device.
- State set/get.
- optional service/call semantics.
- revision/change notification for the UI owner.

The HA core must not know how to speak BLE, Zigbee or Matter.

---

# 21. Entity recipe and controller binding

Each entity recipe defines both presentation semantics and, when supported, a backend binding.

Conceptual structure:

```c
typedef enum {
    APP_BINDING_PASSIVE_VALUE,
    APP_BINDING_BLE_GATT,
    APP_BINDING_ESPHOME_API,
    APP_BINDING_ZIGBEE_ATTRIBUTE,
    APP_BINDING_ZIGBEE_COMMAND,
    APP_BINDING_MATTER_ATTRIBUTE,
    APP_BINDING_MATTER_COMMAND,
    APP_BINDING_NONE,
} app_entity_binding_kind_t;
```

Example BLE binding:

```text
Entity: switch.bot
backend: BLE_GATT
service_uuid: ...
characteristic_uuid: ...
codec_id: ...
readable: true
writable: true
notify: optional
```

Example Zigbee binding:

```text
Entity: switch.outlet
backend: ZIGBEE_COMMAND
endpoint: 1
cluster: OnOff
command: On / Off
```

Example Matter binding:

```text
Entity: light.main
backend: MATTER_ATTRIBUTE + MATTER_COMMAND
endpoint: 1
cluster: OnOff / LevelControl
```

Passive BLE sensor:

```text
Entity: sensor.temperature
backend: PASSIVE_VALUE
decoder: Theengs selected decoder
property_id: temperature
writable: false
```

The binding table is application-owned and keyed by Entity ID. Do not embed another project's public handle inside HA Entity records.

---

# 22. Dynamic entities vs profile-defined entities

Not every Entity must be completely hard-coded in the Device DB.

Use two paths.

## 22.1 Profile-defined entity recipes

Best for:

- BLE broadcast sensors.
- known BLE GATT products.
- vendor-specific Zigbee quirks.
- known proprietary device protocols.

## 22.2 Protocol-enumerated entities

Best for standards that already expose structured capabilities:

- ESPHome Native API entity descriptions.
- Zigbee standard clusters after ZHA normalization.
- Matter endpoint/device-type/cluster data.

The Device DB may provide overrides, display metadata and quirk IDs, but the application should not duplicate authoritative protocol capability enumeration in static tables.

---

# 23. UI workflow

The UI is deliberately generic.

## 23.1 Home / Devices screen

Core elements:

```text
[ Scan ]
Nearby Devices

Device card
Device card
Device card
...
```

Each card is backed by one HA Device.

Recommended card information:

- icon.
- display name/model fallback.
- available/unavailable state.
- optional small protocol/source badge in diagnostics-oriented builds.

Do not create protocol-specific primary screens for normal control.

## 23.2 Device detail screen

Selecting a Device enumerates its HA Entities:

```c
ha_core_entity_for_device(...)
```

or equivalent.

The screen renders each Entity according to HA domain/device class.

Suggested generic widgets:

| HA semantic | UI |
|---|---|
| `sensor` | value + unit, read-only |
| `binary_sensor` | state indicator |
| `switch` | toggle |
| `light` | on/off; brightness/color controls only when supported |
| `button` | press button |
| `number` | bounded slider/input |
| `select` | dropdown |
| `climate` | current/target temp + supported mode controls |
| unavailable/unknown | disabled control + status |

The UI renderer does not inspect protocol IDs.

## 23.3 Entity interaction

When a writable widget changes, UI posts an application control request:

```text
entity_id
requested HA-semantic action/value
```

The UI must not call `zigpy_*`, `chip_*` or `esphome_*` directly.

---

# 24. Control dispatch workflow

Application control dispatcher performs:

```text
UI action
→ lookup HA Entity
→ lookup app Entity binding
→ validate Device availability/authorization
→ convert HA semantic value to backend operation
→ invoke unique owning L2 API
→ wait for confirmed result/report
→ update HA State
→ notify UI
```

## 24.1 BLE GATT

```text
Entity action
→ ESPHome-derived GATT workflow
→ connect if needed
→ discover/cache bounded handles if needed
→ encode value using Device DB codec/recipe
→ read/write
→ optional readback/notification
→ disconnect/retain session according to bounded policy
```

Use `esphome_ble_gatt_*` only.

## 24.2 ESPHome Native API

```text
Entity action
→ esphome_api_command(...)
→ wait for protocol confirmation/state update
→ ha_core_state_set(...)
```

## 24.3 Zigbee

Attribute-backed entity:

```text
Entity action
→ zha_transform_encode() if quirk transform required
→ application copies to zigpy-owned value type
→ zigpy_attr_write_async()
→ response/report
→ zha_transform_decode() if required
→ HA State update
```

Command-backed entity:

```text
Entity action
→ zigpy_command_invoke_async()
→ report/readback
→ HA State update
```

ZHA never directly calls zigpy.

## 24.4 Matter

```text
Entity action
→ chip_write_attribute()
   or chip_invoke()
→ secure CASE/ACL result
→ subscription/readback
→ HA State update
```

## 24.5 Read-only/unsupported

If binding is `PASSIVE_VALUE` or `NONE`, UI control is disabled. The Entity still displays state/information.

---

# 25. State confirmation rule

Do not treat a UI request as confirmed device state.

Preferred state progression:

```text
current
→ pending
→ confirmed new state
```

or:

```text
current
→ pending
→ failed, restore current
```

Sources of confirmation:

- BLE GATT readback or notification.
- ESPHome Native API state message.
- Zigbee write/command response followed by attribute report/readback.
- Matter write/invoke response plus subscription/readback.

Optimistic UI may be used only when explicitly documented for a protocol/profile, and the final HA State must still converge to reported device truth.

---

# 26. Live state update paths

After initial scan, some protocols can keep Entities fresh without rescanning the entire environment.

## Passive BLE

```text
new Kismet BLE advertisement
→ Wireshark parse
→ Device DB known profile lookup/cache
→ Theengs selected decoder
→ HA State update
```

## ESPHome

```text
esphome_api subscription
→ Entity value
→ HA State update
```

## Zigbee

```text
zigpy attribute report
→ optional ZHA transform
→ HA State update
```

## Matter

```text
chip subscription/event
→ HA State update
```

mDNS/SSDP/Nmap/Kismet Wi-Fi primarily refresh discovery/availability metadata unless a Device DB entity recipe explicitly maps some observation to a state Entity.

---

# 27. Unknown and partially known devices

The product should still show useful unknown devices.

Examples:

```text
Unknown BLE Device
- address
- name if advertised
- RSSI
- services
```

```text
Unknown Wi-Fi AP
- SSID/BSSID
- channel
- security
- RSSI
```

```text
Unknown LAN Host
- IP
- hostname if known
- discovered services
```

Unknown Devices are read-only unless an authoritative standard protocol provides safe generic semantics.

A later stronger match may upgrade an existing generic Device to a known profile while preserving the stable application Device ID if an explicit identity rule proves it is the same physical device.

---

# 28. Ambiguous match handling

`AMBIGUOUS` is not an error to hide and is never permission to guess.

On ambiguous profile match:

1. keep generic Device.
2. retain evidence IDs that may support a later refinement.
3. schedule only safe read-only enrichment if it can disambiguate.
4. rerun the relevant typed Device DB match with new evidence.
5. create writable Entity bindings only after a deterministic match or authoritative protocol capability result.

Never choose the first database candidate by ordering.

---

# 29. Radio/resource scheduling

The application is the only cross-family scheduler.

Initial conservative rules:

- one Kismet RF scan session at a time.
- serialize Kismet Wi-Fi passive monitor and Kismet BLE scanning.
- serialize BLE scan and OpenThread discovery.
- serialize BLE scan and Zigbee scan/commissioning where coexistence is not proven.
- do not run Matter BLE commissioning while Kismet owns BLE scanning.
- do not reinitialize NimBLE from two families; define clear native host ownership during implementation.
- restore Wi-Fi STA before mDNS/SSDP/Nmap.
- keep SD database reads bounded because LCD and SD share SPI2 on this board.
- never perform long blocking Device DB I/O while holding the LVGL owner context.

Every L2 operation used by the application must expose or provide a finite timeout/cancel mechanism when it can block.

---

# 30. Memory/bounds requirements

ESP32-C6 has no assumed PSRAM.

Application requirements:

- bounded maximum visible Devices.
- bounded Entities per Device.
- bounded evidence cache per scan generation.
- bounded control requests.
- bounded queue sizes.
- no packet-driven unbounded heap allocation.
- explicit truncation flags.
- stream Device DB records from SD instead of loading the full corpus.
- one/few active GATT/Matter/Nmap transactions at a time.
- avoid retaining raw packets after semantic extraction.

When capacity is exceeded:

- do not overwrite memory.
- increment/drop counters.
- preserve already materialized Devices.
- expose partial/truncated scan status.

---

# 31. Authentication and authorization

Discovery and control are separate.

Allowed automatic scan behavior:

- passive RF observation.
- normal mDNS/SSDP discovery.
- bounded local host/service probes.
- read-only interrogation of already authorized devices where profile policy allows it.

Require explicit user action/credentials for:

- BLE pairing/bonding/passkey.
- Zigbee permit-join.
- Thread dataset attach/joiner.
- Matter commissioning.
- ESPHome Noise key/credential entry when not already available.
- device-specific authenticated control.

Never implement or use:

- credential harvesting/guessing.
- authentication bypass.
- deauthentication/jamming.
- hostile replay/session hijacking.
- poisoning.
- exploit delivery.
- third-party persistent access outside legitimate protocol pairing/commissioning.

---

# 32. Database-to-control trust rule

A database match is allowed to suggest a control binding only when all of the following are true:

1. match is deterministic, not ambiguous;
2. binding comes from approved provenance;
3. operation maps to a documented protocol capability;
4. backend requires normal protocol authorization;
5. parameters are bounded and validated;
6. user action is required for state-changing operations;
7. a failed backend operation cannot accidentally fall through to a different candidate profile.

This rule is essential because a wrong hardware fingerprint combined with a write-capable GATT/Zigbee/Matter binding is more serious than a wrong display name.

---

# 33. Scan progress model

Use explicit stages, for example:

```text
WIFI_RF
BLE_RF
THREAD
ZIGBEE
MDNS
SSDP
LAN_HOSTS
LAN_SERVICES
ENRICHMENT
MATERIALIZE
DONE
```

Each stage reports:

```text
state: pending/running/done/partial/failed/skipped
completed work
optional total work
error code
```

Overall UI progress is derived from terminal stage completion and known work units where meaningful.

Do not fake progress with a timer.

---

# 34. End-to-end pseudocode

The implementation does not have to literally use this function shape, but behavior should be equivalent.

```c
static void nearby_full_scan_run(void)
{
    app_scan_generation_begin();

    /* 1. Wi-Fi RF */
    app_scan_stage_begin(APP_STAGE_WIFI_RF);
    run_kismet_wifi_scan();
    app_scan_stage_end(APP_STAGE_WIFI_RF);

    /* 2. BLE RF */
    app_scan_stage_begin(APP_STAGE_BLE_RF);
    run_kismet_ble_scan();
    app_scan_stage_end(APP_STAGE_BLE_RF);

    /* 3. Thread network discovery, if enabled */
    run_openthread_discovery_if_available();

    /* 4. Existing authorized Zigbee devices */
    run_zigpy_inventory_and_interview_if_available();

    /* 5. Restore/ensure normal STA connectivity */
    app_restore_wifi_sta_and_wait_ip();

    /* 6. LAN advertised discovery */
    run_ha_mdns_discovery();
    run_ha_ssdp_discovery();

    /* 7. LAN active discovery */
    run_nmap_discovery_and_services();

    /* 8. Recognition/refinement */
    app_resolve_pending_device_matches();

    /* 9. Safe read-only probes */
    app_run_safe_enrichment_queue();

    /* 10. Final semantic materialization */
    app_materialize_pending_devices_and_entities();

    app_scan_generation_sweep_stale();
    app_scan_generation_finish();
}
```

BLE event handling conceptually:

```c
static void on_ble_advertisement(const kismet_ble_observation_t *obs)
{
    wireshark_ble_adv_t ad;
    device_db_match_result_t match;

    if (wireshark_ble_adv_parse(obs->payload, obs->payload_len, &ad) != 0) {
        return;
    }

    device_db_ble_fingerprint_t fp;
    app_build_ble_fingerprint(obs, &ad, &fp);

    switch (device_db_match_ble(&g_db, &fp, &match)) {
    case DEVICE_DB_MATCHED:
        app_upsert_matched_ble_device(obs, &ad, &match);
        if (match.theengs_decoder_id != DEVICE_DB_NO_DECODER) {
            app_decode_passive_ble_values(match.theengs_decoder_id, &ad, &match);
        }
        break;

    case DEVICE_DB_AMBIGUOUS:
        app_upsert_generic_ble_device(obs, &ad);
        app_schedule_safe_disambiguation(&fp);
        break;

    case DEVICE_DB_NOT_FOUND:
    default:
        app_upsert_generic_ble_device(obs, &ad);
        break;
    }
}
```

Control dispatch conceptually:

```c
static void app_control_entity(const ha_entity_id_t entity_id,
                               const app_requested_value_t *requested)
{
    app_entity_binding_t binding;

    if (!app_binding_get(entity_id, &binding)) {
        app_control_fail(entity_id, APP_ERR_NOT_CONTROLLABLE);
        return;
    }

    app_control_pending(entity_id);

    switch (binding.kind) {
    case APP_BINDING_BLE_GATT:
        app_control_ble_gatt(&binding, requested);
        break;

    case APP_BINDING_ESPHOME_API:
        app_control_esphome(&binding, requested);
        break;

    case APP_BINDING_ZIGBEE_ATTRIBUTE:
    case APP_BINDING_ZIGBEE_COMMAND:
        app_control_zigbee(&binding, requested);
        break;

    case APP_BINDING_MATTER_ATTRIBUTE:
    case APP_BINDING_MATTER_COMMAND:
        app_control_matter(&binding, requested);
        break;

    default:
        app_control_fail(entity_id, APP_ERR_NOT_CONTROLLABLE);
        break;
    }
}
```

---

# 35. Concrete example — BLE thermometer

```text
Kismet BLE scan
→ address/RSSI/advertisement bytes

Wireshark BLE AD parse
→ service UUID + service data + manufacturer data

Device DB BLE match
→ profile: Xiaomi thermometer
→ decoder: THEENGS_DECODER_X
→ entities: temperature/humidity/battery

Theengs selected decoder
→ temp=24.7 C
→ humidity=57 %
→ battery=83 %

HA core
→ Device Xiaomi Thermometer
→ sensor.temperature = 24.7
→ sensor.humidity = 57
→ sensor.battery = 83

LVGL
→ Device card
→ detail page shows 3 read-only Entities
```

No GATT connection is required unless the profile separately defines useful active features.

---

# 36. Concrete example — BLE controllable device

```text
Kismet BLE
→ Wireshark BLE parse
→ Device DB match
→ known model
→ Device DB entity recipe:
   switch.main
   backend = BLE_GATT
   service UUID + characteristic UUID + codec ID

HA Device/Entity created

User toggles switch
→ UI posts Entity action
→ application binding lookup
→ esphome_ble_gatt_connect/discover/write
→ protocol response/readback
→ HA State confirmed
→ LVGL toggle state updated
```

Theengs is not involved in GATT control unless its selected passive decoder is also used for telemetry.

---

# 37. Concrete example — Zigbee plug

```text
zigpy interview
→ IEEE address
→ manufacturer/model
→ endpoints/clusters

Device DB Zigbee match
→ profile + zha_quirk_id

ZHA apply/normalize
→ switch outlet
→ power sensor
→ voltage sensor
→ energy sensor

HA Device/Entities created

User toggles switch
→ app binding = ZIGBEE_COMMAND
→ zigpy_command_invoke_async(OnOff command)
→ response/report
→ HA State
→ UI
```

For vendor-specific attributes:

```text
zigpy report
→ application copies value into ZHA input
→ zha_transform_decode
→ HA State
```

---

# 38. Concrete example — Matter light

```text
HA mDNS discovery
→ Device DB identifies Matter operational service

Matter controller
→ CASE session
→ matter_node_probe
→ Descriptor / Basic Information / clusters

Device DB Matter refinement
→ profile/device type
→ entity bindings

HA Device
├─ light.main
└─ sensor.power

User changes light
→ chip_invoke or chip_write_attribute
→ Matter response/subscription
→ HA State
→ UI
```

If the device is only commissionable, show it as uncommissioned and require an explicit commissioning action before normal writable Entities become active.

---

# 39. Concrete example — ESPHome node

```text
HA mDNS discovery
→ Device DB identifies ESPHome
→ esphome_api_probe
→ DeviceInfo + capabilities
→ esphome_api_entities
→ HA Device + Entities

state subscription
→ HA State updates

User controls switch/light
→ esphome_api_command
→ state confirmation
→ HA State
→ UI
```

The application does not duplicate those entity descriptions in a separate ESPHome-specific UI.

---

# 40. Testing requirements

## 40.1 Device DB

- exact match vectors per protocol.
- ambiguous/collision vectors.
- missing fields.
- truncated input.
- provenance validation.
- deterministic generated output.
- stable profile/entity IDs across rebuilds unless intentionally migrated.
- bounded SD reads and corrupt DB fail-closed behavior.

## 40.2 Scan workflow

- every stage success.
- every stage timeout.
- cancellation at every stage.
- partial scan.
- Wi-Fi reconnect failure before LAN scan.
- BLE/Thread/Zigbee serialization.
- repeated 100x scan without heap/resource leaks.
- full tables/capacity exhaustion.
- stale-generation cleanup.

## 40.3 Matching/materialization

- unknown → known refinement.
- ambiguous remains non-writable.
- safe identity merge.
- unsafe identity evidence does not merge.
- profile creates correct HA Device and Entities.
- dynamic ESPHome/Zigbee/Matter entities map correctly.

## 40.4 Control

For every writable Entity backend:

- success.
- timeout.
- device unavailable.
- authentication failure.
- malformed value.
- out-of-range value.
- backend disconnect/reboot.
- confirmed state update.
- failed control does not leave false final state.

## 40.5 UI

- scan progress follows real stages.
- Device list can refresh while scanning.
- detail screen uses HA Entity semantics only.
- unavailable Entities are disabled.
- read-only Entities cannot dispatch writes.
- UI/LVGL is mutated only on its owner task.

## 40.6 Target resource tests

On ESP32-C6 with LVGL active:

- firmware size delta per L2 family.
- minimum free heap.
- largest free block.
- relevant task stack high-water marks.
- SD lookup latency.
- scan peak memory.
- GATT/Zigbee/Matter control peak memory.

Do not invent physical measurements.

---

# 41. Implementation order

Integrate in dependency/use order, while keeping Level-2 families independent.

Recommended application build-up:

```text
1. HA semantic core (Device/Entity/State)
2. Device DB engine + tiny fixture database
3. Wireshark BLE/Wi-Fi parsers
4. Kismet Wi-Fi/BLE sessions + trackers
5. BLE scan → DB → HA Device/Entity end-to-end
6. Theengs selected decoder → HA sensor states
7. HA mDNS/SSDP → DB → HA Devices
8. Nmap LAN discovery → DB refinement
9. ESPHome BLE GATT + Native API bindings
10. zigpy interview/ZCL
11. Device DB ZHA fingerprints + ZHA quirk/capability application
12. OpenThread network workflows
13. Matter controller/node probe/read
14. Matter write/invoke/subscribe/commissioning after footprint/security gates
15. final generic UI polish and full integration/resource tests
```

Do not wait for all protocol families before proving the end-to-end model. The first useful vertical slice should be:

```text
Kismet BLE
→ Wireshark BLE parser
→ tiny Device DB
→ Theengs selected decoder
→ HA Device/Entities
→ LVGL list/detail
```

Then add one writable BLE GATT Entity to prove the control dispatcher.

---

# 42. Completion criteria

The Nearby Devices application is complete when the following workflow works without protocol-specific UI glue:

```text
User presses Scan
→ application executes bounded multi-stage environment scan
→ protocol evidence is parsed by unique owning L2 modules
→ integrated Device DB matches/refines devices
→ known profiles select decoders/quirks/controller bindings
→ HA Device + Entity + State records are created
→ Device list appears in UI
→ user opens any Device
→ generic Entity list appears
→ user operates any supported writable Entity
→ application dispatches to the unique owning backend
→ backend confirms/reports state
→ HA State updates
→ UI updates
```

And the same UI path works for at least:

- passive BLE sensor.
- BLE GATT controllable device.
- ESPHome Native API node.
- Zigbee device.
- Matter device.
- LAN-discovered read-only device.

The architecture is successful only if adding a new Device DB profile normally requires **data + an existing decoder/controller binding**, not a new bespoke application screen or a second copy of an already implemented scan/parser/controller.

---

# 43. Rules for Agents implementing this workflow

Before changing application integration code, an Agent must answer these questions:

1. Which unique module owns the capability?
2. Is an equivalent capability already implemented by another family?
3. Is this code Level 1, Level 2, Device DB data, or Nearby Devices application composition?
4. Does the change accidentally make one L2 family call another?
5. Does matching happen through the single Device DB?
6. Is any new writable Entity deterministic and safely bound to the correct physical device?
7. Is storage/memory bounded on ESP32-C6 without PSRAM?
8. Does every asynchronous/active operation have timeout/cancel/error cleanup?
9. Does a UI action update state only after backend confirmation/reporting?
10. Can the same Device/Entity UI render the result without protocol-specific special cases?

If the answer to #2 is yes, do not add the duplicate capability. Use the existing unique owner through application composition.

If the answer to #5 is no, do not create another Runtime matcher database. Add approved source data to the integrated Device DB generator instead.

If the answer to #10 is no, first determine whether the missing concept should be represented as a Home Assistant Entity/attribute rather than adding a protocol-specific screen.
