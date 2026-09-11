# ESPHome L2 provenance

This component is a clean-room One-OS implementation. It does not copy or port ESPHome C/C++ runtime code.

## References

- ESPHome 2026.8.0 BLE/GATT runtime is **REFERENCE-ONLY** for lifecycle and orchestration semantics. ESPHome's C/C++ runtime is GPLv3 and is not incorporated here.
- BLE execution uses ESP-IDF 6.1 ESP-NimBLE directly. Espressif NimBLE central examples were used to verify native connection, discovery, GATT operation and cancellation signatures.
- ESPHome Native API message IDs and protobuf field numbers are derived from `esphome/components/api/api.proto` at ESPHome 2026.8.0. The bounded protobuf/framing code in One-OS is independently implemented.
- Defensive plaintext framing behavior was cross-checked against the MIT-licensed `aioesphomeapi` client behavior.

## Previous-project reuse

No previous NearBy runtime code was reused. This component does not restore `nearby_*`, `radio_runtime`, scan/session/coordinator layers, recognition databases, mDNS discovery, protocol UI, BLE RF scanning, BLE advertisement parsing or passive device decoders.

## Noise transport

The Native API transport implements two framings: the bounded plaintext slice, and the encrypted `Noise_NNpsk0_25519_ChaChaPoly_SHA256` framing that ESPHome peers use when an API encryption key is configured.

`esphome_noise.c` is a clean-room implementation of the Noise Protocol Framework revision 34 (sections 5, 6, 7.5, 9 and 12) for the single pattern ESPHome uses. It contains no third-party protocol code: the state machine, transcript layout, token order and key schedule were written from the specification. `esphome_noise_crypto.c` implements SHA-256, HMAC-SHA256, HKDF, ChaCha20-Poly1305 and X25519 from their specifications, depends only on the C standard library headers, allocates nothing, and is shared unchanged by firmware and host tests.

Interoperability evidence, in descending strength:

- `tests/esphome_l2/test_noise.c` pins the exact handshake-message and transport-frame bytes for a fixed PSK, prologue and ephemeral key. Those fixtures are produced by `tools/reference/noise_nnpsk0_reference.py`, a second implementation of the same handshake written in Python on top of `hashlib` and the `cryptography` package. Agreement is an external check, not self-consistency.
- `tests/esphome_l2/noise_test_responder.c` is a third, minimal responder written directly from the specification and drives the initiator through a full exchange with fresh keys, including transport in both directions.
- Every cryptographic primitive is checked against its published vectors: FIPS 180-4 and RFC 6234 for SHA-256, RFC 4231 for HMAC, RFC 5869 for HKDF, RFC 8439 section 2.8 for ChaCha20-Poly1305, and RFC 7748 (including the 1,000-iteration test) for X25519.
- The wire framing and prologue were cross-checked against the MIT-licensed `aioesphomeapi` client (`_frame_helper/noise.py`: prologue `b"NoiseAPIInit\0\0"`, preamble `0x01`, two-byte big-endian frame length, `0x01` protocol byte then the raw handshake message).
- **Not yet verified against a real ESPHome node.** That is a hardware item, tracked as B7 in `docs/handover-ledger.md`.

Authenticated control now transmits. `esphome_api_command()` sends a command only on an established encrypted session, and returns `ESP_ERR_NOT_SUPPORTED` with `ESPHOME_API_PROTOCOL_ERROR_AUTH_REQUIRED` when no PSK was configured. A successful send is not treated as a state change: observed state still moves only when the peer reports it through the subscription callback. A 32-byte PSK is never logged, guessed, or transmitted; it is mixed into the handshake and wiped.

## Bounded footprint measurements

Host `-Os` object-code measurements for the platform-neutral implementation are 4,616 bytes (`esphome_api.c`), 5,938 bytes (`esphome_api_codec.c`) and 3,274 bytes (`esphome_ble_gatt.c`), 13,828 bytes total. These are host object text sizes only and intentionally exclude the ESP-NimBLE backend and ESP-IDF/lwIP libraries; the ESP32-C6 linker map remains the authoritative firmware measurement.

Runtime storage is caller-bounded: `esphome_api_session_t` is 1,440 bytes (the Noise handshake state and the plaintext receive buffer share one union), `esphome_ble_gatt_session_t` is 512 bytes, GATT discovery arrays are supplied by the caller, subscriptions are fixed at eight slots, and Native API payload storage is capped at 1,024 bytes. The NimBLE notification bridge copies at most 256 bytes per notification and reports truncation.

Noise adds no heap allocation. The handshake state is 240 bytes and each transport cipherstate is 48 bytes, all inside the caller's session. Flash and RAM figures for the Noise code on the ESP32-C6 are **not measured**; the linker map is the only authoritative source and no local toolchain can produce one.
