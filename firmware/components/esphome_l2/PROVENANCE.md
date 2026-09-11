# ESPHome L2 provenance

This component is a clean-room One-OS implementation. It does not copy or port ESPHome C/C++ runtime code.

## References

- ESPHome 2026.8.0 BLE/GATT runtime is **REFERENCE-ONLY** for lifecycle and orchestration semantics. ESPHome's C/C++ runtime is GPLv3 and is not incorporated here.
- BLE execution uses ESP-IDF 6.1 ESP-NimBLE directly. Espressif NimBLE central examples were used to verify native connection, discovery, GATT operation and cancellation signatures.
- ESPHome Native API message IDs and protobuf field numbers are derived from `esphome/components/api/api.proto` at ESPHome 2026.8.0. The bounded protobuf/framing code in One-OS is independently implemented.
- Defensive plaintext framing behavior was cross-checked against the MIT-licensed `aioesphomeapi` client behavior.

## Previous-project reuse

No previous NearBy runtime code was reused. This component does not restore `nearby_*`, `radio_runtime`, scan/session/coordinator layers, recognition databases, mDNS discovery, protocol UI, BLE RF scanning, BLE advertisement parsing or passive device decoders.

## Noise / authenticated control limitation

The current Native API transport implements the bounded plaintext protocol slice only. It detects peers that require encrypted Noise framing and returns an explicit unsupported/encryption-required error. A caller-supplied 32-byte Noise PSK is validated only for shape; it is never copied, logged, guessed or transmitted by this implementation.

Authenticated ESPHome control remains required by this component's scope; `esphome_api_command()` does not transmit commands on the plaintext transport. It validates supported command shapes and returns `ESP_ERR_NOT_SUPPORTED` with `ESPHOME_API_PROTOCOL_ERROR_AUTH_REQUIRED`. Command protobuf encoding is fixture-tested so an authenticated transport can use it later, but enabling device control requires a separately validated Noise client implementation.

## Bounded footprint measurements

Host `-Os` object-code measurements for the platform-neutral implementation are 4,616 bytes (`esphome_api.c`), 5,938 bytes (`esphome_api_codec.c`) and 3,274 bytes (`esphome_ble_gatt.c`), 13,828 bytes total. These are host object text sizes only and intentionally exclude the ESP-NimBLE backend and ESP-IDF/lwIP libraries; the ESP32-C6 linker map remains the authoritative firmware measurement.

Runtime storage is caller-bounded: `esphome_api_session_t` is 1,280 bytes, `esphome_ble_gatt_session_t` is 512 bytes, GATT discovery arrays are supplied by the caller, subscriptions are fixed at eight slots, and Native API payload storage is capped at 1,024 bytes. The NimBLE notification bridge copies at most 256 bytes per notification and reports truncation.

Noise is not present in this implementation, so no speculative Noise flash/RAM estimate is reported. The remaining blocker for authenticated command transport is a separately validated clean-room Noise_NNpsk0_25519_ChaChaPoly_SHA256 implementation with interoperability vectors; until that exists, control stays fail-closed.
