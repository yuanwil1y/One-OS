/*
 * esphome_noise_crypto.h — portable C11 cryptographic primitives for the
 * Noise protocol (XX/IK style handshakes) used by the ESPHome native API
 * layer.
 *
 * This module is deliberately self-contained: it depends only on
 * <stdbool.h>, <stddef.h> and <stdint.h>, performs no heap allocation,
 * keeps no global mutable state, is fully reentrant, and contains no
 * recursion.  It builds unchanged for the host (tests) and for a 32-bit
 * ESP32-C6 / RISC-V target under ESP-IDF.
 *
 * See esphome_noise_crypto.c for the representation comments.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4) */
#define NOISE_CRYPTO_SHA256_BYTES 32u
#define NOISE_CRYPTO_SHA256_BLOCK 64u
typedef struct { uint32_t state[8]; uint64_t bitlen; uint8_t buf[64]; size_t buflen; } noise_sha256_t;
void noise_sha256_init(noise_sha256_t *ctx);
void noise_sha256_update(noise_sha256_t *ctx, const void *data, size_t len);
void noise_sha256_final(noise_sha256_t *ctx, uint8_t out[NOISE_CRYPTO_SHA256_BYTES]);
void noise_sha256(const void *data, size_t len, uint8_t out[NOISE_CRYPTO_SHA256_BYTES]);

/* HMAC-SHA256 (RFC 2104) */
typedef struct { noise_sha256_t inner; uint8_t opad[NOISE_CRYPTO_SHA256_BLOCK]; } noise_hmac_sha256_t;
void noise_hmac_sha256_init(noise_hmac_sha256_t *ctx, const uint8_t *key, size_t key_len);
void noise_hmac_sha256_update(noise_hmac_sha256_t *ctx, const void *data, size_t len);
void noise_hmac_sha256_final(noise_hmac_sha256_t *ctx, uint8_t out[NOISE_CRYPTO_SHA256_BYTES]);
void noise_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[NOISE_CRYPTO_SHA256_BYTES]);

/* HKDF (RFC 5869) with SHA-256 as in the Noise specification */
void noise_hkdf_sha256(const uint8_t *chaining_key, const uint8_t *ikm, size_t ikm_len,
                       uint8_t *out1, uint8_t *out2, uint8_t *out3, unsigned num_outputs);

/* ChaCha20-Poly1305 AEAD (RFC 8439 section 2.8) with a 96-bit nonce */
#define NOISE_CRYPTO_AEAD_KEY_BYTES 32u
#define NOISE_CRYPTO_AEAD_NONCE_BYTES 12u
#define NOISE_CRYPTO_AEAD_TAG_BYTES 16u
/*
 * Buffer contract (the caller must get this right, it is not checked here):
 *   encrypt: `out` must have room for plaintext_len + 16 bytes (ciphertext
 *            followed by the tag); writes exactly that many bytes.
 *   decrypt: `ciphertext_len` includes the 16-byte tag, and `out` must have
 *            room for ciphertext_len - 16 bytes -- the plaintext size -- even
 *            when the call is going to fail.  The full output region is
 *            touched on every path where ciphertext_len >= 16: on success it
 *            receives the plaintext, on authentication failure it is wiped
 *            (so that a caller that ignores the return value cannot read a
 *            frame).  ciphertext_len < 16 returns false and writes nothing,
 *            since there is no plaintext length to wipe.
 *   `out` may not overlap the input, and `ad`/`plaintext` may be NULL when the
 *   corresponding length is 0.
 */
bool noise_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                        const uint8_t *plaintext, size_t plaintext_len, uint8_t *out);
bool noise_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                        const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *out);

/* X25519 (RFC 7748) */
#define NOISE_CRYPTO_DH_BYTES 32u
void noise_x25519_private_key(uint8_t out[32], const uint8_t entropy[32]);
void noise_x25519_public_key(uint8_t out[32], const uint8_t private_key[32]);
void noise_x25519(uint8_t out[32], const uint8_t private_key[32], const uint8_t peer_public_key[32]);

/* Sensitive buffer wipe that the compiler must not remove */
void noise_crypto_wipe(void *data, size_t len);
