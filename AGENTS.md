# One-OS — Kismet Level-2 API Implementation Agent

## Status

Phase 1 research is complete. **Implementation is approved only for the deduplicated RF scan/tracking scope below.**

Read root `README.md`, `docs/research/kismet-l2-api.md`, and the canonical `main` application docs before coding (use `git show origin/main:<path>` if needed):

- `docs/application/nearby-devices-browser-controller.md`
- `docs/application/nearby-devices-product-rules.md`
- `docs/application/provisioning-web-management.md`

## Final unique ownership

Kismet is the product's **RF environment scanner/tracker**.

It owns:

1. Wi-Fi RF observation/session/channel hopping and AP/STA/SSID/probe tracking.
2. BLE RF observation/session and generic nearby-device tracking.

It does **not** own deep packet parsing or device-model recognition.

Use `kismet_wifi_*` and `kismet_ble_*` names.

## Approved Wi-Fi implementation scope

Implement a bounded session/tracker model equivalent to:

```c
kismet_wifi_tracker_create(...);
kismet_wifi_tracker_reset(...);
kismet_wifi_tracker_ingest(...);
kismet_wifi_tracker_get(...);
kismet_wifi_tracker_count(...);

kismet_wifi_session_start(...);
kismet_wifi_session_cancel(...);
kismet_wifi_session_wait(...);
```

The family should add real Kismet-like semantics:

- channel plan/hopping;
- finite observation session;
- AP/STA/SSID/probe identities and relationships;
- first_seen / last_seen / seen_count;
- RSSI/channel statistics;
- explicit capacity/expiry/eviction/drop accounting;
- clean native Wi-Fi state restoration.

Deep 802.11 management/IE parsing belongs to Wireshark. Expose/copy only enough raw/native metadata for the application to pass bytes to `wireshark_*` independently.

## Approved BLE implementation scope

Implement bounded generic BLE scan/tracking equivalent to:

```c
kismet_ble_tracker_create(...);
kismet_ble_tracker_reset(...);
kismet_ble_tracker_ingest(...);
kismet_ble_tracker_get(...);
kismet_ble_session_start(...);
kismet_ble_session_cancel(...);
kismet_ble_session_wait(...);
```

Track generic RF/inventory facts only:

- address/address type;
- RSSI;
- connectable indication;
- first/last seen;
- seen count;
- bounded expiry/eviction.

## Explicitly do NOT implement

- Wi-Fi IE/security deep parser;
- BLE AD/service/manufacturer parser;
- Theengs/model/device fingerprint matching;
- BLE GATT control;
- universal DeviceGraph;
- 802.15.4 tracker for this product phase;
- protocol-specific UI.

Unique owners are Wireshark, Device DB/Theengs, ESPHome and the application.

## Architecture requirements

- L1 stays native ESP-IDF Wi-Fi/NimBLE/FreeRTOS.
- No cross-family calls or public type coupling.
- No generic `nearby_*` compatibility layer.
- Application owns cross-family RF scheduling.
- Every session is bounded, cancellable and restores the native state it changed.

## Testing

Test full tables, eviction, duplicate observations, malformed/short frames passed through, cancellation, repeated start/stop, channel progress and 100x lifecycle/resource stability where practical. Keep ESP32-C6 build green.

## Safety

Passive reconnaissance and ordinary nearby discovery are in scope. No deauthentication, credential capture/cracking, hostile injection/replay, jamming, bypass or hijacking.

## Completion

Implement, test and commit the approved Wi-Fi + BLE session/tracker scope, then report APIs, build/tests, capacities, measured resource use if available and remaining limitations.
