# One-OS — ESPHome Level-2 API Research Agent

## Mission

Research and, only after approval, implement **ESPHome-derived Level-2 APIs** for One-OS on Waveshare ESP32-C6-Touch-LCD-1.9.

Read root `README.md` first. One-OS aims to discover nearby devices, identify them, enumerate usable capabilities, read information/state, and perform legitimate interaction/control.

## API boundary

- L1 stays native: ESP-IDF, NimBLE, lwIP, FreeRTOS, LVGL, IEEE 802.15.4, thin BSP.
- L2 must add real reusable behavior by composing native calls; no one-call renaming wrappers.
- ESPHome family uses `esphome_*` names.
- This family must not call or expose another project API family.
- Do not import ESPHome's YAML/code-generation framework or recreate its whole runtime.

## Starting hypotheses to verify

Research ESPHome capabilities with direct value to One-OS, especially:

- BLE tracker/client/proxy-style discovery and connection workflows;
- service/characteristic enumeration, read/write/notify flows where ESPHome provides meaningful orchestration;
- concrete device/vendor protocol implementations that identify devices or expose sensor/state/control capabilities;
- local-network discovery/control protocols implemented by ESPHome where they can be bounded for ESP32-C6;
- reusable device interrogation/state/control patterns rather than configuration-system features.

Treat these as starting hypotheses only. Determine which capabilities are actually reusable Level-2 APIs and which belong to apps, generators, or ESPHome-specific infrastructure.

## Phase 1 — research only

Do not implement production code. Inspect upstream ESPHome docs/source and relevant previous NearBy/One-OS research.

For each candidate report: upstream source/module; exact behavior; supported device/protocol scope; value to discovery/identification/state/control; proposed `esphome_*` C API/data types; native APIs composed; why it is L2; ESP32-C6 feasibility; memory/bounds; authorization/security requirements; licensing/provenance (`COPY`, `PORT`, `CLEAN-ROOM REIMPLEMENT`, `REFERENCE-ONLY`); disposition (`L2 API`, `APP`, `TEST/TOOL`, `DROP`); test vectors/edge cases.

Pay special attention to whether a device-specific implementation should become a reusable API family capability or remain application/device-plugin data. Avoid creating hundreds of trivial wrappers.

Create `docs/research/esphome-l2-api.md`, finish with a prioritized API table and exclusions, commit it, report findings, then **stop**. Implementation requires explicit user approval.

## Phase 2 — only after explicit approval

Implement only approved APIs with bounded memory/results, explicit timeouts/cancellation where needed, native-state restoration, tests, provenance notes, and a continuously buildable ESP32-C6 tree. No cross-family dependency and no generic `nearby_*` compatibility layer.

## Safety boundary

Authorized discovery, connection, interrogation, subscriptions, reads/writes and device control are in scope. Do not implement credential theft, auth bypass, deauthentication, poisoning, session hijacking, exploit delivery, hostile MITM or persistence on third-party devices.
