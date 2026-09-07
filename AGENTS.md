# One-OS — Kismet Level-2 API Research Agent

## Mission

Research and, only after approval, implement **Kismet-derived Level-2 APIs** for One-OS on Waveshare ESP32-C6-Touch-LCD-1.9.

Read root `README.md` first. One-OS aims to discover as many nearby wireless devices as practical, identify them, track their relationships/state over time, and expose useful information to applications.

## API boundary

- L1 stays native: ESP-IDF Wi-Fi/BLE/802.15.4, FreeRTOS and thin BSP.
- L2 must add meaningful tracking, orchestration, parsing or state semantics; no one-call renaming wrappers.
- Kismet family uses `kismet_*` names.
- It must not call or expose other project API families.
- Do not reproduce Kismet server/Web UI architecture unless a reusable embedded capability clearly requires part of it.

## Starting hypotheses to verify

Investigate Kismet capabilities that may translate well to ESP32-C6:

- Wi-Fi/AP/station/device inventory and relationship tracking;
- SSID/probe/probe-response tracking;
- first-seen/last-seen/signal/channel statistics;
- channel hopping/scheduling and bounded observation sessions;
- BLE or other PHY device tracking where Kismet defines useful reusable semantics;
- packet/device classification metadata useful to applications.

These are hypotheses only. Determine what is genuinely Kismet-derived and what is merely raw native capture or another project's parser model.

## Phase 1 — research only

Do not implement production firmware. Inspect Kismet upstream docs/source and relevant prior One-OS/NearBy work.

For each candidate API document: exact upstream source/behavior; product value; proposed `kismet_*` C API/data types; L1 calls composed; why it is L2; tracking state and eviction policy; ESP32-C6 RAM/flash feasibility; radio scheduling/coexistence impact; bounded session/cancellation behavior; license/provenance (`COPY`, `PORT`, `CLEAN-ROOM REIMPLEMENT`, `REFERENCE-ONLY`); disposition (`L2 API`, `APP`, `TEST/TOOL`, `DROP`); tests and malformed/edge cases.

Avoid inventing an unbounded universal DeviceGraph. Embedded tracking must have explicit capacity, expiry and partial-result semantics.

Create `docs/research/kismet-l2-api.md`, ending with a prioritized API table and exclusions. Commit, report findings, then **stop** until explicit implementation approval.

## Phase 2 — only after explicit approval

Implement only approved APIs with fixed/bounded tracking storage, explicit session lifecycle, clean radio restoration, tests and provenance. Keep the ESP32-C6 build green. No cross-family dependency and no generic `nearby_*` compatibility layer.

## Safety boundary

Passive reconnaissance and ordinary active discovery are in scope. Do not implement deauthentication, credential capture/cracking, security bypass, hostile injection/replay, jamming, session hijacking, exploit delivery or persistence.
