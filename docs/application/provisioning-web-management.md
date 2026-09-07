# Nearby Devices — Provisioning / Web Management Migration Guide

Status: **normative application-layer implementation guide**  
Target: **Waveshare ESP32-C6-Touch-LCD-1.9 / ESP32-C6 / ESP-IDF / FreeRTOS / LVGL**  
Product: **Nearby Devices Browser / Controller**

This document defines how Wi-Fi provisioning and Device DB import work in the final product, how they appear in the on-device Settings UI, and which parts of the previous NearBy One NEXT implementation should be reused directly versus adapted to the clean One-OS architecture.

It must be read together with:

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`

The product requirement is intentionally simple:

```text
Settings
→ user chooses Provision / Import Database
→ device starts temporary SoftAP + Web Management service
→ user connects from phone/tablet/computer
→ browser opens Web Management
   ├─ Wi-Fi tab: choose SSID + enter password + save provisioning
   └─ Database tab: upload devices.nbdb
→ device validates and persists result
→ portal stops
→ normal Nearby Devices runtime resumes
```

The previous NearBy One NEXT already implemented most of this product surface and should be used as the migration baseline.

---

# 1. Final on-device Settings screen

The Settings page keeps the previous NearBy One NEXT visual layout and Home Assistant-like style.

Required cards/actions:

```text
← Settings

Wi-Fi
<current Wi-Fi status>
<SSID / IP when connected>

[ Provision / Import Database ]

Recognition database
<Ready · version / Missing / Invalid>

Firmware
<firmware version>
```

The exact wording may be compacted to fit 170 × 320, but these four concepts are mandatory:

1. current Wi-Fi state;
2. one user action that starts SoftAP + Web Management for provisioning and DB import;
3. database presence/validity/version;
4. firmware version.

The preferred action is a single primary button:

```text
Web Management
```

or:

```text
Provision / Import DB
```

because the same SoftAP portal exposes both the Wi-Fi and Database tabs.

Do not create separate Wi-Fi-provisioning and DB-import network stacks. Both functions use the same temporary Web Management session.

---

# 2. Wi-Fi status shown in Settings

The Settings page should display an application-owned Wi-Fi status snapshot.

Recommended fields:

```c
typedef enum {
    APP_WIFI_UNCONFIGURED,
    APP_WIFI_DISCONNECTED,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_ERROR,
} app_wifi_state_t;

typedef struct {
    app_wifi_state_t state;
    char ssid[33];
    char ipv4[16];
    int8_t rssi;
    bool credentials_present;
} app_wifi_status_t;
```

UI examples:

```text
Wi-Fi
Connected · HomeWiFi
192.168.1.42
```

```text
Wi-Fi
Configured · Disconnected
```

```text
Wi-Fi
Not configured
```

The UI does not call ESP-IDF Wi-Fi APIs directly. The application owner updates a small status snapshot and asks the LVGL owner to refresh the Settings screen.

---

# 3. Persistent Wi-Fi provisioning

The final product differs from the old NearBy One NEXT in one important way:

**Wi-Fi provisioning is persistent.**

The previous portal described the connection as session-only. That behavior must not be copied unchanged.

Final provisioning workflow:

```text
user starts Web Management
→ temporary SoftAP starts
→ browser selects/scans SSID
→ user enters password
→ POST /api/wifi/connect
→ firmware validates lengths/input
→ save STA credentials to application-owned NVS namespace
→ attempt STA connection while AP remains available
→ report success/failure to browser
→ on success, keep credentials persisted
→ stop portal when user closes it or after explicit successful-completion action
→ normal boot/runtime later reconnects using stored credentials
```

Recommended NVS ownership:

```text
namespace: nearby_wifi
keys:
  ssid
  password
  configured/version marker
```

Do not store credentials in Device DB or SD.

Do not expose the saved Wi-Fi password through status APIs, logs, LVGL labels or HTTP status responses.

A future "Forget Wi-Fi" action, if added, must be explicit and separate from ordinary provisioning.

---

# 4. Temporary SoftAP / Web Management session

The Web Management session is a product-level competing operation.

Initial rules:

- do not start while a full RF scan or commissioning/control workflow owns conflicting radio state;
- starting Web Management pauses/rejects new scan/control requests that would conflict;
- use Wi-Fi `APSTA` mode so the temporary SoftAP can remain available while testing a new STA connection;
- use a temporary WPA2 password generated per session;
- show SoftAP SSID, password and AP IP on the device screen/modal;
- stop the HTTP server and SoftAP cleanly when the session ends;
- restore/continue the provisioned STA state after portal shutdown.

Suggested SoftAP naming continues the old pattern:

```text
NearBy-One-XXXX
```

where `XXXX` is derived from local MAC bytes or another non-secret unique suffix.

Suggested AP address remains the ESP-IDF default unless product testing changes it:

```text
192.168.4.1
```

The old code's random WPA2 password approach is suitable to reuse.

---

# 5. Web Management portal layout

Reuse the previous NearBy One NEXT portal structure and visual design.

The portal remains one static single-page Web UI with two tabs:

```text
NearBy One NEXT
Web Management

[ Wi-Fi ] [ Database ]
```

## 5.1 Wi-Fi tab

Required controls:

```text
Connect to Wi-Fi
[ Scan nearby networks ]

network list:
  SSID
  RSSI
  channel

Network name
[ SSID input ]

Password
[ password input ]

[ Save & Connect ]

status text
```

The previous HTML/CSS and browser-side Wi-Fi network rendering may be reused almost directly.

Change old wording from:

```text
Connect for this session
```

to persistent provisioning wording such as:

```text
Save & Connect
```

The browser should poll or request `/api/status` after starting a connection so it can show the actual STA result rather than merely "connection started".

## 5.2 Database tab

Required controls:

```text
Recognition database
[ choose devices.nbdb ]
[ upload progress ]
[ validation/status ]
[ Upload / Replace Database ]
```

The old `.nbdb` client-side preflight validation logic is useful and can be migrated when its schema matches the new Device DB format.

However, do **not** copy the old "format entire SD card" product behavior.

The SD card is now long-lived product storage and the production Device DB is only one file on it.

Final database-import behavior is:

```text
browser uploads new file
→ firmware streams it to /nearby/db/devices.nbdb.part
→ exact byte count checked
→ full DB validation/checksum/schema/provenance checks
→ flush + fsync
→ close current DB reader / block DB lookup briefly
→ promote validated .part to devices.nbdb atomically/safely
→ reopen DB
→ update Settings DB status/version
→ delete stale .part on any failure
```

Never erase unrelated SD contents.

---

# 6. Canonical HTTP API

The old HTTP API shape is a strong starting point.

Recommended final endpoints:

```text
GET  /api/status
GET  /api/wifi/scan
POST /api/wifi/connect
POST /api/db/upload
GET  /
```

Optional endpoints if useful:

```text
POST /api/portal/finish
POST /api/wifi/forget       // future explicit action only
GET  /api/db/status
```

Remove old destructive-format endpoints from the final portal:

```text
/api/db/preflight-result
/api/db/format
```

unless they are reused only in a completely separate factory/service tool. They are not part of normal Device DB import.

## 6.1 GET /api/status

Return only non-secret status:

```json
{
  "active": true,
  "ap_ssid": "NearBy-One-A1B2",
  "ap_ipv4": "192.168.4.1",
  "sta_state": "connected",
  "sta_ssid": "HomeWiFi",
  "sta_ipv4": "192.168.1.42",
  "db_state": "ready",
  "db_version": 42,
  "firmware": "v0.x.y"
}
```

Never return SoftAP or STA passwords after initial on-device presentation.

## 6.2 GET /api/wifi/scan

Use native ESP-IDF Wi-Fi scan facilities from the Web Management application operation.

Return a bounded list, for example maximum 16 or 32 networks:

```json
[
  {"ssid_hex":"...","rssi":-54,"channel":6,"auth":3}
]
```

Keeping `ssid_hex` avoids malformed UTF-8/escaping issues and is compatible with the old portal pattern.

## 6.3 POST /api/wifi/connect

Input:

```text
application/x-www-form-urlencoded
ssid=<...>&password=<...>
```

Processing:

```text
validate
→ persist credentials to NVS
→ set STA config
→ connect
→ asynchronously update status
```

If the connection fails, the saved credentials may remain available for retry unless the user replaces them. The UI must report failure honestly.

## 6.4 POST /api/db/upload

Input body is raw `.nbdb` bytes.

Streaming requirements:

- no whole-file RAM buffering;
- fixed 1–4 KiB receive/write chunk;
- maximum content length must be bounded by configured DB/file-size policy and available SD space;
- abort/cancel removes `.part`;
- final promotion occurs only after validation succeeds.

---

# 7. Device DB replacement and live reader coordination

Because the main application can read the Device DB while the portal uploads a replacement, the application must coordinate the final promotion.

Simplest first implementation:

```text
start Web Management
→ block new environment scans / DB matching
→ existing UI/HA Device list remains usable read-only where possible
→ upload .part
→ validate
→ close device_db handle
→ rename/promote
→ reopen device_db
→ refresh DB status/version
→ end Web Management
```

Do not attempt complex concurrent hot-swap while scan workers are seeking through the same file in the first implementation.

Existing HA Devices/Entities do not need to be deleted when the DB is replaced. The new DB applies on the next recognition/refinement/full scan, unless an explicit "re-match current devices" feature is added later.

---

# 8. On-device Web Management modal

Reuse the old concept when the portal starts.

Show a modal/card like:

```text
Web Management

Connect to:
NearBy-One-A1B2

Password:
XXXXXXXX

Open:
192.168.4.1

[ Stop Portal ]
```

The modal is informational; Wi-Fi credentials and DB files are entered/uploaded on the user's phone/tablet/computer browser.

The device does not need an on-screen keyboard for Wi-Fi provisioning.

---

# 9. Old NearBy One NEXT code reuse matrix

The previous repository is the preferred implementation source for product/UI/Web code. Reuse is allowed because it is the project's own predecessor code, but the architecture boundaries below must be respected.

| Old file/module | Reuse level | What to reuse | What must change |
|---|---|---|---|
| `components/nearby_ui/nearby_ui.c` | **DIRECT PORT / ADAPT** | 170×320 layout, HA blue palette, top bar, white rounded cards, Device list, Entity rows, Settings layout, Web Management modal | replace old product callbacks/types where new HA/App types differ; add Wi-Fi status card; keep new generic Entity model |
| `components/nearby_ui/include/nearby_ui.h` | **ADAPT** | UI callback model and status update functions | rename/restructure only as needed for new app; avoid reintroducing generic framework wrappers |
| `firmware/components/web_mgmt/web_mgmt.c` | **PORT STRUCTURE / ADAPT** | temporary SoftAP/APSTA setup, random WPA2 password, esp_http_server setup, bounded form parsing, Wi-Fi scan handler, upload streaming pattern | remove dependencies on old `radio_runtime` / `scan_session`; use native ESP-IDF + app operation gate; make Wi-Fi persistent; remove whole-SD format flow |
| `firmware/components/web_mgmt/include/web_mgmt.h` | **ADAPT** | portal status shape/start-stop concept | no old wrapper types; do not expose passwords in general status; reflect persistent Wi-Fi state |
| `firmware/main/nearby_web_portal.c` | **DIRECT PORT** | embedded static `index.html` serving and route registration | path/component naming only |
| `web-portal/index.html` | **DIRECT PORT / ADAPT** | CSS/theme, two-tab layout, network list, form handling, progress/status patterns, client-side `.nbdb` preflight ideas | change session-only provisioning text; remove destructive SD format warning/flow; update DB schema/path/API/status polling |
| `firmware/components/db_storage/db_storage.c` | **PORT SELECTIVELY** | `.part` streaming, exact byte accounting, flush/fsync, validation hook, cancel cleanup, final rename concept | delete `prepare_whole_sd`/format authorization path; use `/nearby/db/devices.nbdb`; coordinate live DB close/reopen; improve replacement safety |
| `firmware/components/db_storage/include/db_storage.h` | **ADAPT** | upload begin/write/finish/cancel API shape | remove destructive-format API; update paths and types |
| `firmware/main/main.c` | **REFERENCE / SELECTIVE PORT** | owner-queue concept, portal start/stop callback flow, firmware version via `esp_app_get_description()`, DB status refresh | do not restore old scan pipeline/radio/session architecture; split product logic into new Nearby Devices app files |

---

# 10. Old code that must not be resurrected merely for reuse convenience

Do not copy these architectural dependencies into One-OS just because old Web Management used them:

```text
radio_runtime
scan_session
scan_coordinator
nearby_scan_pipeline
native_handoff
old nearby_* compatibility API layer
```

The new product already has a clear model:

```text
application operation/state gate
+ native ESP-IDF Wi-Fi/HTTP/NVS/storage APIs
+ independent project L2 families for actual scanner/controller capabilities
```

The portal is application infrastructure, not another Level-2 project family.

---

# 11. Suggested new source placement

A practical final layout is:

```text
firmware/
├─ components/
│  ├─ board/
│  ├─ lvgl_port/
│  └─ ... independent L2 families ...
│
└─ main/
   ├─ nearby_devices_app.c
   ├─ nearby_devices_ui.c
   ├─ nearby_devices_settings.c
   ├─ nearby_devices_wifi.c
   ├─ nearby_devices_web_mgmt.c
   ├─ nearby_devices_web_portal.c
   ├─ nearby_devices_db_storage.c
   └─ ...

web-portal/
└─ index.html
```

If `web_mgmt` becomes a component, keep it explicitly product/application infrastructure; do not present it as a reusable protocol L2 family.

---

# 12. Settings UI data flow

The Settings screen is always populated from application snapshots, never by blocking hardware calls in LVGL callbacks.

Conceptual refresh:

```text
application worker
├─ app_wifi_get_status()
├─ device_db_get_status()
└─ esp_app_get_description()->version
       ↓
APP_EVENT_SETTINGS_STATUS
       ↓
LVGL / HA owner task
       ↓
refresh Settings cards
```

Recommended UI content:

```text
Wi-Fi
Connected · HomeWiFi
192.168.1.42

[ Web Management ]

Recognition database
Ready · v42

Firmware
v0.x.y
```

If DB is missing:

```text
Recognition database
Missing · generic discovery only
```

If Wi-Fi is unconfigured:

```text
Wi-Fi
Not configured
```

---

# 13. Startup behavior with persisted Wi-Fi

At normal boot:

```text
init board/LVGL/storage
→ load saved Wi-Fi credentials from NVS
→ if configured, start/maintain STA connection
→ open SD Device DB if present
→ start Nearby Devices UI
```

Wi-Fi failure must not prevent local RF scanning or Unknown-device display.

LAN stages (mDNS/SSDP/Nmap/ESPHome Native API/Matter IP) are skipped or marked unavailable until STA has an IP.

This status should be visible in Settings.

---

# 14. Interaction with full environment scan

Starting Web Management is mutually exclusive with the full environment scan in the first implementation.

Recommended behavior:

```text
if scan running:
    disable/reject Web Management button

if Web Management active:
    disable Scan and conflicting control/commissioning operations
```

Existing Device/Entity pages may remain viewable while the portal is active, but state-changing operations that require conflicting Wi-Fi/BLE/802.15.4 ownership should be disabled.

After the portal ends:

```text
refresh Wi-Fi status
refresh DB status
resume normal STA state
allow Scan again
```

If a new DB was imported, the next full scan uses it automatically.

---

# 15. Database upload safety rules

Normal DB upload is not destructive to unrelated SD files.

Required checks before promotion:

- expected magic;
- supported container/schema version;
- file/range bounds;
- index offsets and lengths;
- checksum/hash;
- reader ABI compatibility;
- provenance metadata required by the Device DB format;
- all candidate record/index accesses stay inside file bounds.

The firmware-side validator is authoritative even if the browser already performed client-side validation.

Never trust browser-side validation alone.

If upload/power/network fails:

```text
old devices.nbdb remains valid where possible
new devices.nbdb.part is removed on recovery/startup
```

A stronger replacement sequence may use `.old`/backup rename if FATFS rename semantics and failure tests show it is necessary; choose the exact scheme after hardware fault-injection tests.

---

# 16. UI reuse rule

For the device-side LVGL UI, **layout consistency with the previous project is preferred over redesign**.

Agents should first port the old visual structure, then make only the changes required by the new product model:

- preserve top bar/progress strip/card spacing/color language;
- preserve Device list → Device detail navigation;
- preserve generic switch/light/button Entity controls where applicable;
- add new HA domains only when needed;
- add Wi-Fi status to Settings;
- retain Recognition database and Firmware cards;
- retain Web Management button/modal;
- replace old fixed scanner/product assumptions with the new Nearby Devices workflow.

Do not spend Phase 2 inventing a new visual design system.

---

# 17. Agent checklist before implementation

An Agent implementing provisioning/Web Management must confirm all of the following:

1. Did I reuse the old UI/Web implementation where it already solves the same product problem?
2. Did I avoid bringing back old `radio_runtime` / `scan_session` architecture?
3. Does Wi-Fi provisioning persist credentials in NVS?
4. Does SoftAP remain available while a new STA connection is being tested?
5. Are passwords excluded from logs/status APIs?
6. Does the same portal handle both Wi-Fi and DB import?
7. Does DB import replace only `/nearby/db/devices.nbdb`, not format the SD card?
8. Is upload streamed directly to `.part` with fixed buffers?
9. Is firmware-side DB validation mandatory before promotion?
10. Is the Device DB closed/reopened safely around replacement?
11. Does Settings show current Wi-Fi state, DB state/version and firmware version?
12. Does Web Management start/stop through the application owner rather than an LVGL callback doing blocking work?
13. Is Scan disabled/rejected while Web Management owns conflicting state?
14. Does missing Wi-Fi or missing DB still allow the core Nearby Devices UI and RF discovery to operate?
15. Does the device-side UI still look and navigate like the previous NearBy One NEXT Home Assistant-style layout?

If any answer is no, the provisioning/Web Management integration is not complete.
