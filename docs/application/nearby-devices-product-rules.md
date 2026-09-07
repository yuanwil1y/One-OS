# Nearby Devices Browser / Controller — Product Runtime Rules

Status: **normative application-layer supplement**  
Target: **Waveshare ESP32-C6-Touch-LCD-1.9 / ESP32-C6 / ESP-IDF / FreeRTOS / LVGL**  
Application: **Nearby Devices Browser / Controller**

This document supplements `docs/application/nearby-devices-browser-controller.md` and freezes three product requirements that every integration Agent must preserve:

1. the production Device DB lives on the SD card, not in firmware flash;
2. unknown / unmatched devices are never discarded merely because recognition failed or the database is unavailable;
3. the product UI follows a Home Assistant-like Device → Entity experience and visually references the previous NearBy One NEXT UI baseline.

If an older recommendation in the main application guide conflicts with this document, this document wins for these three areas.

---

# 1. Non-negotiable product behavior

The complete product path remains:

```text
L2 scanners / discovery APIs
→ protocol evidence
→ protocol parser where needed
→ SD Device DB match when available
→ known profile OR generic unknown Device
→ HA Device
→ HA Entity / State
→ Home Assistant-like LVGL UI
→ user selects Device
→ user selects/operates Entity
→ application dispatches to the unique owning L2 controller
→ backend confirmation/report
→ HA State update
→ UI refresh
```

The following must always be true:

- SD/DB presence improves recognition; it is not required for the scanner itself to function.
- `NOT_FOUND` is a normal result, not a reason to drop the observation.
- `AMBIGUOUS` is displayed generically and remains non-writable until resolved.
- DB missing/corrupt/incompatible does not make the Device list empty.
- the primary UI never exposes protocol-specific application pages for normal control.
- a user always interacts with `Device → Entity`, regardless of BLE, Wi-Fi/LAN, Zigbee, Matter or ESPHome backend.

---

# 2. Production Device DB must live on SD

## 2.1 Canonical path

The production hardware/profile database is stored at:

```text
/nearby/db/devices.nbdb
```

The firmware image must not embed the full production recognition corpus.

The reason is architectural, not optional optimization:

- ESP32-C6 flash is valuable for executable protocol/runtime code;
- the device corpus will grow much faster than firmware logic;
- the board already provides microSD storage;
- Device DB data should be independently replaceable/versioned without recompiling all firmware;
- the target assumes no PSRAM, so the corpus must also not be copied wholesale into RAM.

## 2.2 What may remain in firmware flash

Firmware may contain only small fixed logic/data needed to operate without the DB:

- DB file-format/schema constants;
- the DB reader/index engine;
- protocol-generic fallback labels/icons;
- generic HA Device/Entity fallback recipes for unknown observations;
- small protocol enum/string tables required by runtime code;
- host/unit-test fixtures in test builds only.

Production firmware must not ship a second hidden full recognition database as a fallback.

There is exactly one production recognition corpus: the SD database.

## 2.3 Boot behavior

Boot must not fail because the SD or DB is unavailable.

Recommended state model:

```text
DEVICE_DB_READY
DEVICE_DB_MISSING
DEVICE_DB_SD_MISSING
DEVICE_DB_CORRUPT
DEVICE_DB_INCOMPATIBLE
DEVICE_DB_IO_ERROR
```

At boot:

```text
board / SD initialization
→ attempt device_db_open("/nearby/db/devices.nbdb")
→ validate header/schema/index/checksum
→ cache only minimal metadata/index state
→ continue application startup regardless of recognition result
```

If DB is unavailable:

```text
scan still works
→ L2 evidence still becomes generic Devices
→ known-profile enrichment is skipped
→ DB status appears in Settings/diagnostics
```

Do not show a blocking modal that prevents scanning simply because the database is missing.

## 2.4 Bounded runtime I/O

The runtime must never load the entire database into RAM.

Use:

- fixed-size header/index structures;
- indexed seeks into the SD file;
- fixed-size read buffers;
- small bounded page/cache slots only if measurements justify them;
- caller-owned result records;
- explicit `BUFFER_TOO_SMALL`, `TRUNCATED`, `IO_ERROR`, `CORRUPT` results.

A practical initial design is:

```text
open DB once
→ validate metadata
→ retain compact top-level index/root offsets
→ typed match call computes bucket/key
→ seek/read only candidate records
→ compare candidates
→ copy one bounded result
```

No runtime JSON parser, Python, Lua, JavaScript or recursive rule VM is allowed.

## 2.5 LCD/SD shared SPI rule

On the target board LCD and SD share `SPI2_HOST`.

Therefore Device DB reads must be:

- short and bounded;
- performed outside the LVGL owner critical path;
- scheduled through the application worker/storage path;
- copied into RAM before posting UI events;
- never performed while holding LVGL locks or inside an LVGL event callback.

The UI should remain responsive while SD lookups occur.

## 2.6 Database updates

The database is independently replaceable.

A future updater/importer must use an atomic file workflow such as:

```text
receive new DB
→ write /nearby/db/devices.nbdb.part
→ validate schema/length/index/checksum/provenance metadata
→ close/sync
→ replace old file atomically where filesystem semantics permit
→ reopen and report version
```

A failed update must not cause memory corruption or cause a malformed DB to become writable control metadata.

## 2.7 DB identity metadata

The DB header should expose at least:

```text
magic
format version
schema version
content version
build timestamp or source revision ID
record count
index offsets/sizes
content checksum
optional provenance-manifest checksum
```

UI Settings should display a compact status such as:

```text
Recognition DB
v2026.09.07 · Ready
```

or:

```text
Recognition DB
Missing — generic discovery only
```

---

# 3. Device DB remains the only recognition matcher

All source projects feed one generated DB at build/data-generation time.

Examples of source material:

```text
Home Assistant matcher knowledge
ZHA / zha-device-handlers fingerprints and declarative quirks
ESPHome device/protocol facts
Theengs protocol/device facts after license-safe clean-room derivation
Matter VID/PID/device type metadata
Zigbee manufacturer/model data
vendor documentation
One-OS-owned captures and fixtures
```

Runtime must not do this:

```text
ha_match()
→ esphome_match()
→ theengs_match()
→ zha_match()
→ choose one
```

Runtime does this:

```text
protocol evidence
→ device_db_match_<protocol>()
→ MATCHED / AMBIGUOUS / NOT_FOUND / DB_ERROR
```

A successful profile may reference implementation IDs such as:

```text
theengs_decoder_id
zha_quirk_id
BLE GATT codec/binding ID
ESPHome Native API binding
Matter profile refinement metadata
entity recipe range
```

The matcher decides identity; the selected L2 backend performs its own protocol semantics after selection.

---

# 4. Unknown and unmatched devices are first-class product objects

## 4.1 Never discard on NOT_FOUND

This is a hard rule:

```text
L2 scanner saw a device
→ parser produced usable evidence
→ Device DB returned NOT_FOUND
→ KEEP THE DEVICE
```

Do not filter it from the Device list.

Do not count it only in scan statistics.

Do not require a vendor/model match to create an HA Device.

The purpose of Nearby Devices is to show the environment, including devices not yet recognized by our corpus.

## 4.2 Generic HA Device materialization

Every sufficiently distinct observation should be materialized as a generic HA-style Device when no profile matches.

Use protocol-appropriate stable keys where safely available.

Examples:

### Unknown BLE

```text
Device name fallback priority:
1. advertised local name
2. "Unknown BLE Device"

metadata:
- address / address type
- RSSI
- connectable
- service UUIDs
- manufacturer/company ID if present
- TX power if present
- first/last seen
```

### Unknown Wi-Fi

```text
Device name fallback priority:
1. SSID
2. "Hidden Wi-Fi AP"
3. "Unknown Wi-Fi Device"

metadata:
- BSSID/MAC
- AP/client/unknown role when observed
- channel
- RSSI
- security summary
- first/last seen
```

### Unknown LAN host

```text
Device name fallback priority:
1. hostname
2. mDNS/SSDP instance name
3. IP address
4. "Unknown LAN Device"

metadata:
- IP address
- hostname
- mDNS services
- SSDP type/USN where available
- Nmap-discovered ports/services
```

### Unknown Zigbee device

Only already-authorized/local-network devices are included automatically.

```text
metadata:
- IEEE EUI-64
- manufacturer/model strings if interview returned them
- endpoints
- clusters
- availability
```

### Unknown Matter device

```text
metadata:
- commissionable vs operational
- VID/PID when legitimately advertised/read
- device type when available
- node/fabric identity only for authorized nodes
- endpoints/clusters after authorized node probe
```

### Thread

Thread networks/topology are primarily infrastructure evidence. Do not invent HA Devices for every Thread network unless the application later defines a specific UI reason. Matter-over-Thread physical devices enter through Matter semantics.

## 4.3 Generic read-only diagnostic Entities

Unknown Devices may expose generic read-only diagnostic Entities when the value has clear protocol meaning.

Examples:

```text
sensor.signal_strength
sensor.channel
sensor.tx_power
sensor.ip_address
sensor.hostname
sensor.last_seen
sensor.service_count
```

These fallback Entities are application-defined generic HA semantic records, not database profiles.

Do not create a writable Entity from a guess.

## 4.4 Unknown Device detail metadata

Not every useful field needs to be an Entity. The Device detail screen may contain a read-only `Device information` section for metadata such as:

```text
Protocol
Address / BSSID / IEEE address
IP
Manufacturer ID
Service UUID list
Port/service list
Zigbee cluster list
Matter VID/PID
First seen / Last seen
Recognition status
```

This information is diagnostic/presentation metadata and does not create a control path.

## 4.5 Ambiguous devices

`AMBIGUOUS` behaves like unknown for safety:

```text
show Device
show evidence
mark recognition = Ambiguous
allow safe read-only enrichment
no writable binding
```

Never pick the first matching database record.

## 4.6 DB failure behavior

`DB_ERROR`, `SD_MISSING`, `CORRUPT` and `INCOMPATIBLE` must degrade to generic presentation:

```text
scan evidence
→ generic Device
→ generic diagnostics
```

A database failure must not discard the device.

## 4.7 Later upgrade from Unknown to Known

When later evidence or a newer DB resolves a generic Device:

```text
Unknown Device
→ deterministic profile match
→ preserve application Device identity when safe identity key proves same physical device
→ enrich manufacturer/model/icon
→ add profile-defined Entities
→ attach decoder/controller bindings
→ keep existing generic diagnostic information where useful
```

Do not destroy/recreate the Device unnecessarily if identity is stable, because that causes UI flicker and state loss.

---

# 5. UI visual direction: Home Assistant-like, based on old NearBy One NEXT

The old NearBy One NEXT UI is the visual baseline, not a requirement to copy every old implementation detail.

The previous UI already established a suitable 170×320 portrait language:

```text
screen: 170 × 320 portrait
top bar height: ~34 px
progress strip: ~3 px
background: light gray-blue
cards: white, rounded, thin border
primary accent: Home Assistant blue
text: dark blue-gray
muted text: gray-blue
```

The old source used approximately:

```text
HA blue      #03A9F4
background   #F4F7F9
text         #263238
muted        #607D8B
border       #E3E8EC
progress bg  #B3E5FC
progress fg  #0277BD
```

These values are a strong starting point. Small visual changes are allowed if they improve readability on the physical display, but the overall product should continue to feel like a compact Home Assistant device browser.

## 5.1 Global design principles

- light theme by default;
- blue primary action/accent;
- white information/entity cards;
- rounded corners;
- minimal shadows;
- clear spacing suitable for touch;
- strong hierarchy: Device name first, protocol/debug info secondary;
- generic HA-domain controls instead of protocol-specific controls;
- no dense packet-analyzer UI in the primary product flow.

## 5.2 Home / Device list

Recommended structure:

```text
┌────────────────────────────┐
│ [Scan]   Nearby Devices  ⚙ │
├────────────────────────────┤
│ scan progress              │
├────────────────────────────┤
│ ┌────────────────────────┐ │
│ │ icon  Living Room Plug│ │
│ │       Zigbee · Online │ │
│ └────────────────────────┘ │
│ ┌────────────────────────┐ │
│ │ icon  Xiaomi Sensor   │ │
│ │       BLE · -61 dBm   │ │
│ └────────────────────────┘ │
│ ┌────────────────────────┐ │
│ │ ?     Unknown BLE     │ │
│ │       BLE · -74 dBm   │ │
│ └────────────────────────┘ │
└────────────────────────────┘
```

Every card corresponds to one HA Device.

Known and unknown devices share the same card system.

Unknown devices must not be hidden in a separate debug-only screen.

A subtle recognition/protocol badge is acceptable:

```text
BLE
Wi-Fi
LAN
Zigbee
Matter
ESPHome
Unknown
```

but the protocol badge is secondary to Device identity/name.

## 5.3 Device card naming

Name priority:

```text
1. Device DB display name / authoritative protocol name
2. advertised/device-reported name
3. hostname / SSID where appropriate
4. protocol-specific Unknown fallback
```

Subtitle priority can include:

```text
vendor + model
protocol + availability
signal strength
uncommissioned / unavailable / ambiguous
```

Do not show raw internal IDs as the main label unless absolutely no better fallback exists.

## 5.4 Scan interaction

Keep the successful old behavior concept:

```text
press Scan
→ progress strip appears under top bar
→ progress reflects real scan stages
→ Device list may update as devices are materialized
```

The old project blocked touch during its fixed scan. The new application may retain full touch blocking initially for deterministic integration, or later allow safe browsing while scan work continues, but it must never allow conflicting commissioning/control actions while radio ownership makes them unsafe.

At minimum:

- Scan cannot be started twice;
- control actions that conflict with active RF stages are disabled or queued;
- progress is based on stage/work completion, not a timer.

## 5.5 Device detail screen

Selecting a Device opens one generic detail page:

```text
←  Device Name

Manufacturer · Model
Protocol / availability

[ Entity card ]
[ Entity card ]
[ Entity card ]

Device information
Address ...
Last seen ...
Recognition ...
```

Known and unknown Devices use the same detail screen.

Known Device:

```text
entity cards first
then optional diagnostics
```

Unknown Device:

```text
read-only generic diagnostic Entities
then Device information evidence
```

Do not create `BLE Detail`, `Zigbee Detail`, `Matter Detail` primary screens.

## 5.6 Entity cards

Follow Home Assistant semantic domains.

Suggested rendering:

| HA domain/semantic | UI treatment |
|---|---|
| `sensor` | name + value + unit |
| `binary_sensor` | name + state icon/text |
| `switch` | toggle |
| `light` | toggle, then brightness/color only if supported |
| `button` | press button |
| `number` | bounded slider/value editor |
| `select` | dropdown/list selector |
| `climate` | current/target temperature + mode controls |
| unavailable | disabled card/control + muted state |
| pending control | spinner/pending marker; do not pretend confirmed |

The old UI used white ~154 px wide cards, ~10 px corner radius, thin border, dark name text and HA-blue state/control accents. Preserve that family resemblance.

## 5.7 Control confirmation

When the user changes an Entity:

```text
UI widget
→ pending state
→ app control dispatcher
→ owning L2 API
→ confirmation/report/readback
→ HA State update
→ UI confirmed state
```

If the operation fails:

```text
pending
→ failed
→ restore previous confirmed state
→ show small error feedback
```

Do not permanently move the UI to an unconfirmed state just because the user tapped a toggle.

## 5.8 Settings screen

Keep it minimal and HA-like.

Recommended items:

```text
Recognition Database
  status/version/path

Storage
  SD mounted/free space

Firmware
  version/build

Optional Web Management
  only if/when that feature is implemented
```

Recognition DB status must clearly distinguish:

```text
Ready
Missing
SD missing
Corrupt
Incompatible
I/O error
```

The user should understand that `Missing` means reduced recognition, not that scanning is broken.

---

# 6. End-to-end behavior with SD DB present

Example BLE sensor:

```text
Kismet BLE scan
→ Wireshark BLE AD parse
→ device_db_match_ble() reads indexed SD candidates
→ MATCHED profile + selected decoder ID
→ Theengs selected decoder
→ HA Device
→ HA sensor Entities
→ LVGL Home Assistant-style Device card
```

Example Zigbee plug:

```text
zigpy interview
→ device_db_match_zigbee() on SD
→ profile + zha_quirk_id
→ ZHA capability normalization
→ HA Device/Entities
→ LVGL cards
→ user toggles switch Entity
→ zigpy command/write
→ report/confirmation
→ HA State/UI
```

---

# 7. End-to-end behavior with SD DB missing

The application must still be useful:

```text
Kismet BLE scan
→ Wireshark parse
→ device_db_match_ble() = DB unavailable
→ generic BLE Device
→ read-only signal/address/services metadata
→ visible Device card
```

```text
Kismet Wi-Fi scan
→ Wireshark management parse
→ generic Wi-Fi Device/AP
→ SSID/BSSID/channel/security/RSSI
→ visible Device card
```

```text
HA mDNS/SSDP + Nmap
→ generic LAN Device
→ host/IP/services
→ visible Device card
```

Existing authorized protocol nodes can still expose protocol-authoritative information that does not depend on recognition data, but no Device DB-derived writable recipe may be invented while DB identity is unavailable.

---

# 8. Required application data structures

The exact C names may change, but the application needs an explicit recognition state independent of Device availability.

Conceptually:

```c
typedef enum {
    APP_RECOGNITION_MATCHED,
    APP_RECOGNITION_AMBIGUOUS,
    APP_RECOGNITION_UNKNOWN,
    APP_RECOGNITION_DB_UNAVAILABLE,
} app_recognition_state_t;
```

Per Device application metadata should retain bounded fields such as:

```text
app_device_id
HA device id
source protocols bitset
recognition state
matched profile id when present
availability
first/last seen
primary signal value where relevant
small diagnostic evidence summary
```

Raw packet payloads must not be retained after semantic extraction unless a specific diagnostics feature explicitly requests a bounded capture.

---

# 9. Unknown-device capacity policy

Unknown devices can be numerous. They must not be discarded merely for being unknown, but the fixed-memory device table still needs capacity rules.

When capacity is full:

1. never overflow memory;
2. apply the same bounded eviction policy to known and unknown ephemeral observations according to documented priority/age rules;
3. never silently treat `unknown` as lower-value solely because it is unmatched;
4. never evict persistent authorized Matter/Zigbee controller identities before ephemeral RF observations;
5. surface `partial/truncated` scan status and drop counters.

This distinguishes **resource-bounded eviction** from **recognition-based filtering**. The former is necessary; the latter is forbidden.

---

# 10. Agent implementation checklist

An Agent touching Nearby Devices integration must verify all of the following:

1. Is the production Device DB read from `/nearby/db/devices.nbdb` on SD?
2. Did the change accidentally copy the full recognition corpus into firmware flash or RAM?
3. Does scanning continue if SD or the DB is unavailable?
4. Does `NOT_FOUND` still produce a visible generic Device?
5. Does `AMBIGUOUS` remain visible and non-writable?
6. Are unknown basic fields exposed without inventing unsupported capabilities?
7. Are Device DB reads bounded and outside LVGL owner callbacks?
8. Does every Device—known or unknown—use the same generic Device-detail UI?
9. Are controls rendered from HA Entity semantics rather than protocol-specific screens?
10. Does visual styling remain consistent with the old NearBy One NEXT / Home Assistant-like blue-white card language?
11. Are state-changing controls dispatched only to the unique owning L2 backend?
12. Is final UI state based on backend confirmation/reporting?

A change that makes unmatched devices disappear, makes the UI depend on DB availability, or embeds the production recognition corpus into firmware violates this product specification.
