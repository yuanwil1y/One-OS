# One-OS — OpenThread Level-2 API Research Agent

## Mission

Research and, only after approval, implement **OpenThread-derived Level-2 APIs** for One-OS on Waveshare ESP32-C6-Touch-LCD-1.9.

Read root `README.md` first. One-OS aims to discover nearby Thread networks/devices, inspect topology/state, join or commission authorized networks, and expose useful diagnostics/interactions.

## API boundary

- L1 stays native: ESP-IDF/OpenThread integration facilities, IEEE 802.15.4, FreeRTOS, lwIP, thin BSP.
- L2 must combine native facilities into meaningful reusable workflows; no one-call wrappers.
- OpenThread family uses `openthread_*` names.
- It must not call Matter, Home Assistant, ZHA, Kismet or other One-OS project API families.

## Starting hypotheses to verify

Investigate:

- active/passive Thread network discovery and scan result interpretation;
- join/attach/commissioning workflows where authorized;
- neighbor/router/child/topology inspection;
- network diagnostics and dataset/state reporting;
- channel/PAN/Extended PAN/RSSI/LQI metadata useful for discovery;
- operational state transitions and resource/lifecycle handling that justify L2 APIs.

These are hypotheses only. Determine what OpenThread itself provides versus what belongs to Matter or ESP-IDF integration layers.

## Phase 1 — research only

Do not implement production firmware. Inspect OpenThread upstream source/docs, ESP-IDF integration details and relevant previous One-OS/NearBy work.

For each candidate API document: exact upstream source/API/CLI behavior; value to discovery/state/control; proposed `openthread_*` C API/types; L1 calls composed; why it is L2; ESP32-C6 feasibility; radio coexistence constraints; RAM/flash/bounds; dataset/persistence implications; authorization/security requirements; license/provenance (`COPY`, `PORT`, `CLEAN-ROOM REIMPLEMENT`, `REFERENCE-ONLY`); disposition (`L2 API`, `APP`, `TEST/TOOL`, `DROP`); tests/edge cases.

Clearly separate OpenThread network capabilities from Matter application-layer capabilities.

Create `docs/research/openthread-l2-api.md`, ending with a prioritized API table and exclusions. Commit, report findings, then **stop** until explicit implementation approval.

## Phase 2 — only after explicit approval

Implement only approved APIs with bounded state/results, explicit timeouts, clean attach/detach lifecycle, restoration of temporary radio state, tests and provenance. Keep the ESP32-C6 build green and avoid cross-family dependencies or generic `nearby_*` abstractions.

## Safety boundary

Authorized scanning, joining, commissioning, diagnostics and network inspection are in scope. Do not implement key extraction, unauthorized network entry, security bypass, jamming, hostile replay, session hijacking, exploit delivery or persistence.
