# One-OS — Matter / CHIP Tool Level-2 API Research Agent

## Mission

Research and, only after approval, implement **Project CHIP / CHIP Tool-derived Level-2 APIs** for One-OS on Waveshare ESP32-C6-Touch-LCD-1.9.

Read root `README.md` first. One-OS aims to discover Matter devices, identify capabilities, read/subscribe to state, commission authorized devices, and invoke legitimate controls.

## API boundary

- L1 stays native: ESP-IDF networking/BLE/802.15.4 facilities, FreeRTOS, lwIP and thin BSP.
- L2 must provide meaningful Matter workflows/model behavior; no trivial wrappers.
- Prefer project-derived `chip_*` naming when mirroring CHIP Tool/controller workflows. Use `matter_*` only when the API represents protocol semantics rather than a CHIP Tool-specific workflow; explain every naming choice in the report.
- This family must not depend on Home Assistant, OpenThread, ZHA or other One-OS project API families. Apps may compose them.

## Starting hypotheses to verify

Investigate:

- commissionable-node discovery over BLE/IP;
- commissioning and fabric/session lifecycle suitable for an embedded controller;
- endpoint/cluster discovery;
- attribute read/write and subscriptions;
- event subscriptions/reads;
- command invocation;
- device/cluster metadata needed to expose controllable capabilities;
- diagnostics or operational discovery useful after commissioning.

Treat all of these as hypotheses. Determine what is feasible on ESP32-C6 without importing an oversized host controller stack unchanged.

## Phase 1 — research only

Do not write production firmware. Inspect connectedhomeip/CHIP Tool upstream source/docs and relevant previous One-OS/NearBy work.

For each candidate API document: exact upstream source/module/command semantics; protocol prerequisite; value to discovery/state/control; proposed `chip_*`/`matter_*` C API/types; L1 facilities required; why it is L2; required Matter stack footprint; ESP32-C6 RAM/flash feasibility; credential/fabric/persistence requirements; concurrency/radio interactions; license/provenance (`COPY`, `PORT`, `CLEAN-ROOM REIMPLEMENT`, `REFERENCE-ONLY`); disposition (`L2 API`, `APP`, `DATA`, `TEST/TOOL`, `DROP`); interoperability/negative tests.

Explicitly identify which operations require user ownership/commissioning credentials and which are public discovery only.

Create `docs/research/matter-chip-tool-l2-api.md`, ending with a prioritized API table, footprint risks and exclusions. Commit, report findings, then **stop** until explicit user approval.

## Phase 2 — only after explicit approval

Implement only approved APIs with bounded buffers/state, explicit timeouts and error states, secure credential handling, tests, provenance notes and continuous ESP32-C6 buildability. Do not route this family through OpenThread or Home Assistant APIs; use native/platform facilities directly.

## Safety boundary

Public discovery plus authorized commissioning, attribute access, subscriptions and command/control are in scope. Do not bypass commissioning/security, steal fabric credentials, hijack sessions, exploit devices, or persist on third-party devices.
