#pragma once
/* Clean-room Noise_NNpsk0_25519_ChaChaPoly_SHA256 handshake and transport.
 *
 * Scope: this module implements the Noise protocol state machine exactly as
 * specified in the Noise Protocol Framework, revision 34, sections 5, 6, 7.5,
 * 9 and 12, instantiated as Noise_NNpsk0_25519_ChaChaPoly_SHA256. It owns no
 * sockets, no tasks and no timers; the caller moves bytes and the state
 * machine reports what it wants next. Cryptographic primitives live in
 * esphome_noise_crypto.c.
 *
 * Fixed pattern (the only pattern ESPHome Native API uses):
 *
 *   NN:          -> e
 *                <- e, ee
 *   NNpsk0:      -> psk, e
 *                <- e, ee
 *
 * Prologue: the caller supplies it. ESPHome uses the 12 raw bytes
 * "NoiseAPIInit" with no NUL terminator (verified against aioesphomeapi
 * _frame_helper/noise.py, which calls set_prologue(b"NoiseAPIInit\0\0")). The
 * empty prologue is a valid, different protocol context and is used by the
 * module's own conformance vectors.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esphome_noise_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire constants of the ESPHome Native API noise transport. */
#define ESPHOME_NOISE_PREAMBLE             0x01u
#define ESPHOME_NOISE_HANDSHAKE_MSG_BYTES  48u /* 32-byte dh public + 16-byte tag */
#define ESPHOME_NOISE_TAG_BYTES            NOISE_CRYPTO_AEAD_TAG_BYTES
#define ESPHOME_NOISE_MAX_OVERHEAD_BYTES   (2u + 4u + ESPHOME_NOISE_TAG_BYTES)
/* "NoiseAPIInit", 12 bytes, no terminator. */
#define ESPHOME_NOISE_PROLOGUE             "NoiseAPIInit"
#define ESPHOME_NOISE_PROLOGUE_LEN         12u
/* Noise_<pattern>_25519_ChaChaPoly_SHA256 is 43 bytes, so per section 5.2 the
 * initial h/ck is SHA-256 of the name rather than the zero-padded name. */
#define ESPHOME_NOISE_PROTOCOL_NAME        "Noise_NNpsk0_25519_ChaChaPoly_SHA256"

typedef enum {
    ESPHOME_NOISE_STEP_DONE = 0,   /* nothing to do, or handshake complete */
    ESPHOME_NOISE_STEP_WRITE,      /* the initiator must send handshake msg 1 */
    ESPHOME_NOISE_STEP_READ,       /* the initiator must receive handshake msg 2 */
    ESPHOME_NOISE_STEP_FAILED      /* terminal: the session must be discarded */
} esphome_noise_step_t;

typedef enum {
    ESPHOME_NOISE_OK = 0,
    ESPHOME_NOISE_ERR_ARG,          /* NULL or zero-length argument */
    ESPHOME_NOISE_ERR_STATE,        /* call is invalid for the current state */
    ESPHOME_NOISE_ERR_SIZE,         /* caller buffer too small, or wrong length */
    ESPHOME_NOISE_ERR_AUTH_FAILED,  /* AEAD authentication failed */
    ESPHOME_NOISE_ERR_PUBLIC_KEY    /* peer sent a null/low-order key */
} esphome_noise_err_t;

/* Randomness is injected; the module never calls a platform RNG itself and
 * never keeps a global generator. entropy_user is passed through unchanged. */
typedef void (*esphome_noise_entropy_fn)(void *entropy_user, uint8_t *out, size_t len);

/* Noise CipherState (section 5.1). Key, nonce and a flag; no padding. */
typedef struct {
    uint8_t key[NOISE_CRYPTO_AEAD_KEY_BYTES];
    uint64_t nonce;
    bool has_key;
} esphome_noise_cipherstate_t;

/* Noise HandshakeState (section 5.3) for the NNpsk0 pattern.
 * No heap, no internal pointers, no padding. */
typedef struct {
    uint8_t h[NOISE_CRYPTO_SHA256_BYTES];        /* handshake hash */
    uint8_t ck[NOISE_CRYPTO_SHA256_BYTES];       /* chaining key */
    uint8_t k[NOISE_CRYPTO_AEAD_KEY_BYTES];      /* current cipher key */
    uint8_t psk[NOISE_CRYPTO_AEAD_KEY_BYTES];    /* pre-shared key */
    uint8_t e_priv[NOISE_CRYPTO_DH_BYTES];       /* local ephemeral private */
    uint8_t re[NOISE_CRYPTO_DH_BYTES];           /* remote ephemeral public */
    uint8_t prologue[ESPHOME_NOISE_PROLOGUE_LEN];
    uint64_t nonce;                              /* handshake cipher nonce */
    esphome_noise_entropy_fn entropy;
    void *entropy_user;
    /* 0 = not started, 1 = expecting write, 2 = expecting read, 3 = complete */
    uint8_t phase;
    uint8_t prologue_len;
    bool has_psk;
    bool has_k;
    bool fixed_ephemeral;   /* e_priv was supplied by a conformance vector */
} esphome_noise_handshake_t;

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

/* Initialise the initiator half of Noise_NNpsk0_25519_ChaChaPoly_SHA256.
 * psk_len must be 32. prologue may be NULL only when prologue_len is 0.
 * entropy must be non-NULL and non-NULL entropy_user is not required. */
esphome_noise_err_t esphome_noise_initiator_init(esphome_noise_handshake_t *hs,
                                                 const uint8_t *psk, size_t psk_len,
                                                 const uint8_t *prologue, size_t prologue_len,
                                                 esphome_noise_entropy_fn entropy,
                                                 void *entropy_user);

/* Override the ephemeral scalar for deterministic conformance vectors. Only
 * legal before the first write_message. entropy_seed is 32 bytes and is
 * clamped internally. Test and vector use only. */
esphome_noise_err_t esphome_noise_set_fixed_ephemeral(esphome_noise_handshake_t *hs,
                                                      const uint8_t entropy_seed[32]);

/* What the initiator must do next. */
esphome_noise_step_t esphome_noise_step(const esphome_noise_handshake_t *hs);

/* Write the first handshake message (48 bytes) into out. */
esphome_noise_err_t esphome_noise_write_message(esphome_noise_handshake_t *hs,
                                                uint8_t *out, size_t out_cap, size_t *out_len);

/* Read the responder's handshake message, which must be exactly 48 bytes.
 * in is not modified. On ESPHOME_NOISE_ERR_AUTH_FAILED the PSK or the peer's
 * key material is wrong and the session is terminal. */
esphome_noise_err_t esphome_noise_read_message(esphome_noise_handshake_t *hs,
                                               const uint8_t *in, size_t in_len);

/* Section 5.3 Split(). After a successful read_message the initiator gets
 * send = cipher key 1, recv = cipher key 2. Both outputs are required. */
esphome_noise_err_t esphome_noise_split(esphome_noise_handshake_t *hs,
                                        esphome_noise_cipherstate_t *send,
                                        esphome_noise_cipherstate_t *recv);

/* 32-byte handshake hash for channel binding, available after split. */
esphome_noise_err_t esphome_noise_handshake_hash(const esphome_noise_handshake_t *hs,
                                                 uint8_t out[32]);

/* Wipe all key material. Safe to call at any time, including twice. */
void esphome_noise_handshake_wipe(esphome_noise_handshake_t *hs);

/* ------------------------------------------------------------------ */
/* Transport (section 5.1 EncryptWithAd / DecryptWithAd)               */
/* ------------------------------------------------------------------ */

/* Initialise a transport cipher with a 32-byte key, nonce 0. */
esphome_noise_err_t esphome_noise_cipherstate_init(esphome_noise_cipherstate_t *cs,
                                                   const uint8_t key[32]);

/* plaintext_len bytes must fit in out with 16 bytes of tag appended. */
esphome_noise_err_t esphome_noise_encrypt(const esphome_noise_cipherstate_t *cs,
                                          const uint8_t *ad, size_t ad_len,
                                          const uint8_t *plaintext, size_t plaintext_len,
                                          uint8_t *out, size_t out_cap, size_t *out_len);

/* ciphertext_len includes the 16-byte tag; out receives ciphertext_len - 16
 * bytes. Returns ESPHOME_NOISE_ERR_AUTH_FAILED when the tag is wrong, without
 * consuming a nonce. */
esphome_noise_err_t esphome_noise_decrypt(const esphome_noise_cipherstate_t *cs,
                                          const uint8_t *ad, size_t ad_len,
                                          const uint8_t *ciphertext, size_t ciphertext_len,
                                          uint8_t *out, size_t out_cap, size_t *out_len);

/* Section 11.3 REKEY(). */
esphome_noise_err_t esphome_noise_cipherstate_rekey(esphome_noise_cipherstate_t *cs);

void esphome_noise_cipherstate_wipe(esphome_noise_cipherstate_t *cs);

#ifdef __cplusplus
}
#endif
