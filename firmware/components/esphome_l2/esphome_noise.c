/* Clean-room Noise_NNpsk0_25519_ChaChaPoly_SHA256 state machine.
 * See include/esphome_noise.h for scope and the exact specification sections.
 * No sockets, no heap, no globals, no recursion. */
#include "esphome_noise.h"

#include <string.h>

#define PHASE_IDLE 0u
#define PHASE_WRITE 1u
#define PHASE_READ 2u
#define PHASE_DONE 3u

/* ------------------------------------------------------------------ */
/* SymmetricState (Noise section 5.2)                                  */
/* ------------------------------------------------------------------ */

static void mix_hash(esphome_noise_handshake_t *hs, const uint8_t *data, size_t len)
{
    noise_sha256_t sha;
    noise_sha256_init(&sha);
    noise_sha256_update(&sha, hs->h, NOISE_CRYPTO_SHA256_BYTES);
    if (len != 0u) {
        noise_sha256_update(&sha, data, len);
    }
    noise_sha256_final(&sha, hs->h);
    noise_crypto_wipe(&sha, sizeof(sha));
}

/* Inject the pre-shared key into both ck and h and derive a new k. */
static void mix_key_and_hash(esphome_noise_handshake_t *hs, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t new_ck[32];
    uint8_t temp_h[32];
    uint8_t temp_k[32];

    noise_hkdf_sha256(hs->ck, ikm, ikm_len, new_ck, temp_h, temp_k, 3u);
    memcpy(hs->ck, new_ck, sizeof(new_ck));
    mix_hash(hs, temp_h, sizeof(temp_h));
    memcpy(hs->k, temp_k, sizeof(temp_k));
    hs->nonce = 0u;
    hs->has_k = true;

    noise_crypto_wipe(new_ck, sizeof(new_ck));
    noise_crypto_wipe(temp_h, sizeof(temp_h));
    noise_crypto_wipe(temp_k, sizeof(temp_k));
}

/* ck, temp_k = HKDF(ck, ikm, 2); k = temp_k; n = 0 */
static void mix_key(esphome_noise_handshake_t *hs, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t new_ck[32];
    uint8_t temp_k[32];

    noise_hkdf_sha256(hs->ck, ikm, ikm_len, new_ck, temp_k, NULL, 2u);
    memcpy(hs->ck, new_ck, sizeof(new_ck));
    memcpy(hs->k, temp_k, sizeof(temp_k));
    hs->nonce = 0u;
    hs->has_k = true;

    noise_crypto_wipe(new_ck, sizeof(new_ck));
    noise_crypto_wipe(temp_k, sizeof(temp_k));
}

static void nonce_bytes(uint64_t nonce, uint8_t out[NOISE_CRYPTO_AEAD_NONCE_BYTES])
{
    /* Noise section 12.3: ChaChaPoly uses 32 zero bits then a little-endian
     * encoding of the 64-bit nonce. */
    memset(out, 0, 4u);
    for (unsigned i = 0; i < 8u; i++) {
        out[4u + i] = (uint8_t)((nonce >> (8u * i)) & 0xffu);
    }
}

/* EncryptAndHash for a zero-length payload: 16 tag bytes and MixHash(tag). */
static esphome_noise_err_t encrypt_empty_payload(esphome_noise_handshake_t *hs,
                                                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t nonce[NOISE_CRYPTO_AEAD_NONCE_BYTES];

    if (out_cap < NOISE_CRYPTO_AEAD_TAG_BYTES) {
        return ESPHOME_NOISE_ERR_SIZE;
    }
    if (hs->nonce == UINT64_MAX) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    nonce_bytes(hs->nonce, nonce);
    if (!noise_aead_encrypt(hs->k, nonce, hs->h, NOISE_CRYPTO_SHA256_BYTES, NULL, 0u, out)) {
        return ESPHOME_NOISE_ERR_AUTH_FAILED;
    }
    hs->nonce++;
    mix_hash(hs, out, NOISE_CRYPTO_AEAD_TAG_BYTES);
    *out_len = NOISE_CRYPTO_AEAD_TAG_BYTES;
    return ESPHOME_NOISE_OK;
}

/* DecryptAndHash for a zero-length payload. */
static esphome_noise_err_t decrypt_empty_payload(esphome_noise_handshake_t *hs,
                                                 const uint8_t *in)
{
    uint8_t nonce[NOISE_CRYPTO_AEAD_NONCE_BYTES];
    uint8_t scratch[NOISE_CRYPTO_AEAD_TAG_BYTES]; /* the payload plaintext is zero bytes */

    if (hs->nonce == UINT64_MAX) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    nonce_bytes(hs->nonce, nonce);
    /* The AEAD API requires a non-NULL output even for a zero-length
     * plaintext, so the tag is verified against a scratch buffer and the
     * (empty) decrypted result is discarded. */
    if (!noise_aead_decrypt(hs->k, nonce, hs->h, NOISE_CRYPTO_SHA256_BYTES, in,
                            NOISE_CRYPTO_AEAD_TAG_BYTES, scratch)) {
        return ESPHOME_NOISE_ERR_AUTH_FAILED;
    }
    noise_crypto_wipe(scratch, sizeof(scratch));
    hs->nonce++;
    mix_hash(hs, in, NOISE_CRYPTO_AEAD_TAG_BYTES);
    return ESPHOME_NOISE_OK;
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

esphome_noise_err_t esphome_noise_initiator_init(esphome_noise_handshake_t *hs,
                                                 const uint8_t *psk, size_t psk_len,
                                                 const uint8_t *prologue, size_t prologue_len,
                                                 esphome_noise_entropy_fn entropy,
                                                 void *entropy_user)
{
    static const char name[] = ESPHOME_NOISE_PROTOCOL_NAME;
    noise_sha256_t sha;

    if (hs == NULL || psk == NULL || entropy == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (psk_len != NOISE_CRYPTO_AEAD_KEY_BYTES) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (prologue == NULL && prologue_len != 0u) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (prologue_len > ESPHOME_NOISE_PROLOGUE_LEN) {
        return ESPHOME_NOISE_ERR_SIZE;
    }

    esphome_noise_handshake_wipe(hs);
    hs->entropy = entropy;
    hs->entropy_user = entropy_user;

    /* InitializeSymmetric(protocol_name). The name is longer than HASHLEN. */
    noise_sha256(name, sizeof(name) - 1u, hs->h);
    memcpy(hs->ck, hs->h, NOISE_CRYPTO_SHA256_BYTES);
    /* InitializeKey(empty): k is the zeroed array, has_k stays false. */

    /* Always MixHash the prologue, including the empty one. */
    hs->prologue_len = (uint8_t)prologue_len;
    if (prologue_len != 0u) {
        memcpy(hs->prologue, prologue, prologue_len);
    }
    mix_hash(hs, hs->prologue, prologue_len);

    memcpy(hs->psk, psk, NOISE_CRYPTO_AEAD_KEY_BYTES);
    hs->has_psk = true;
    hs->phase = PHASE_WRITE;

    /* h is public and is deliberately retained for channel binding after
     * split(); ck, k and psk stay internal until then. */
    noise_crypto_wipe(&sha, sizeof(sha));
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_set_fixed_ephemeral(esphome_noise_handshake_t *hs,
                                                      const uint8_t entropy_seed[32])
{
    if (hs == NULL || entropy_seed == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (hs->phase != PHASE_WRITE) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    noise_x25519_private_key(hs->e_priv, entropy_seed);
    hs->fixed_ephemeral = true;
    return ESPHOME_NOISE_OK;
}

esphome_noise_step_t esphome_noise_step(const esphome_noise_handshake_t *hs)
{
    if (hs == NULL) {
        return ESPHOME_NOISE_STEP_FAILED;
    }
    switch (hs->phase) {
    case PHASE_WRITE: return ESPHOME_NOISE_STEP_WRITE;
    case PHASE_READ: return ESPHOME_NOISE_STEP_READ;
    case PHASE_DONE: return ESPHOME_NOISE_STEP_DONE;
    default: return ESPHOME_NOISE_STEP_FAILED;
    }
}

esphome_noise_err_t esphome_noise_write_message(esphome_noise_handshake_t *hs,
                                                uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t e_pub[NOISE_CRYPTO_DH_BYTES];

    if (hs == NULL || out == NULL || out_len == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    *out_len = 0u;
    if (hs->phase != PHASE_WRITE) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    if (out_cap < ESPHOME_NOISE_HANDSHAKE_MSG_BYTES) {
        return ESPHOME_NOISE_ERR_SIZE;
    }

    /* NNpsk0 message 1: psk, e, payload */
    mix_key_and_hash(hs, hs->psk, NOISE_CRYPTO_AEAD_KEY_BYTES);
    if (!hs->fixed_ephemeral) {
        hs->entropy(hs->entropy_user, hs->e_priv, NOISE_CRYPTO_DH_BYTES);
        noise_x25519_private_key(hs->e_priv, hs->e_priv);
    }
    noise_x25519_public_key(e_pub, hs->e_priv);
    memcpy(out, e_pub, NOISE_CRYPTO_DH_BYTES);
    /* The "e" token in a PSK handshake is MixHash(e.public_key) followed by
     * MixKey(e.public_key) (Noise section 9.2). */
    mix_hash(hs, e_pub, NOISE_CRYPTO_DH_BYTES);
    mix_key(hs, e_pub, NOISE_CRYPTO_DH_BYTES);

    {
        size_t payload_len = 0u;
        esphome_noise_err_t err = encrypt_empty_payload(
            hs, out + NOISE_CRYPTO_DH_BYTES, out_cap - NOISE_CRYPTO_DH_BYTES, &payload_len);
        if (err != ESPHOME_NOISE_OK) {
            hs->phase = PHASE_IDLE;
            return err;
        }
        *out_len = NOISE_CRYPTO_DH_BYTES + payload_len;
    }

    hs->phase = PHASE_READ;
    noise_crypto_wipe(e_pub, sizeof(e_pub));
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_read_message(esphome_noise_handshake_t *hs,
                                               const uint8_t *in, size_t in_len)
{
    uint8_t dh[NOISE_CRYPTO_DH_BYTES];
    esphome_noise_err_t err;

    if (hs == NULL || in == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (hs->phase != PHASE_READ) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    if (in_len != ESPHOME_NOISE_HANDSHAKE_MSG_BYTES) {
        hs->phase = PHASE_IDLE;
        return ESPHOME_NOISE_ERR_SIZE;
    }

    /* NNpsk0 message 2: e, ee, payload */
    memcpy(hs->re, in, NOISE_CRYPTO_DH_BYTES);
    mix_hash(hs, hs->re, NOISE_CRYPTO_DH_BYTES);
    mix_key(hs, hs->re, NOISE_CRYPTO_DH_BYTES);

    noise_x25519(dh, hs->e_priv, hs->re);
    mix_key(hs, dh, sizeof(dh));
    noise_crypto_wipe(dh, sizeof(dh));

    err = decrypt_empty_payload(hs, in + NOISE_CRYPTO_DH_BYTES);
    if (err != ESPHOME_NOISE_OK) {
        hs->phase = PHASE_IDLE;
        return err;
    }

    hs->phase = PHASE_DONE;
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_split(esphome_noise_handshake_t *hs,
                                        esphome_noise_cipherstate_t *send,
                                        esphome_noise_cipherstate_t *recv)
{
    uint8_t k1[32];
    uint8_t k2[32];

    if (hs == NULL || send == NULL || recv == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (hs->phase != PHASE_DONE) {
        return ESPHOME_NOISE_ERR_STATE;
    }

    noise_hkdf_sha256(hs->ck, NULL, 0u, k1, k2, NULL, 2u);
    (void)esphome_noise_cipherstate_init(send, k1);
    (void)esphome_noise_cipherstate_init(recv, k2);

    noise_crypto_wipe(k1, sizeof(k1));
    noise_crypto_wipe(k2, sizeof(k2));
    /* h is public, but every key in the handshake state is now dead. */
    {
        uint8_t keep_h[NOISE_CRYPTO_SHA256_BYTES];
        memcpy(keep_h, hs->h, sizeof(keep_h));
        esphome_noise_handshake_wipe(hs);
        memcpy(hs->h, keep_h, sizeof(keep_h));
        hs->phase = PHASE_DONE;
        noise_crypto_wipe(keep_h, sizeof(keep_h));
    }
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_handshake_hash(const esphome_noise_handshake_t *hs,
                                                 uint8_t out[32])
{
    if (hs == NULL || out == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (hs->phase != PHASE_DONE) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    memcpy(out, hs->h, NOISE_CRYPTO_SHA256_BYTES);
    return ESPHOME_NOISE_OK;
}

void esphome_noise_handshake_wipe(esphome_noise_handshake_t *hs)
{
    if (hs == NULL) {
        return;
    }
    noise_crypto_wipe(hs, sizeof(*hs));
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

esphome_noise_err_t esphome_noise_cipherstate_init(esphome_noise_cipherstate_t *cs,
                                                   const uint8_t key[32])
{
    if (cs == NULL || key == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    noise_crypto_wipe(cs, sizeof(*cs));
    memcpy(cs->key, key, NOISE_CRYPTO_AEAD_KEY_BYTES);
    cs->nonce = 0u;
    cs->has_key = true;
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_encrypt(const esphome_noise_cipherstate_t *cs,
                                          const uint8_t *ad, size_t ad_len,
                                          const uint8_t *plaintext, size_t plaintext_len,
                                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t nonce[NOISE_CRYPTO_AEAD_NONCE_BYTES];
    esphome_noise_cipherstate_t *mutable_cs = (esphome_noise_cipherstate_t *)cs;

    if (cs == NULL || out == NULL || out_len == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    *out_len = 0u;
    if (!cs->has_key) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    if (cs->nonce == UINT64_MAX) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    if (out_cap < plaintext_len + NOISE_CRYPTO_AEAD_TAG_BYTES) {
        return ESPHOME_NOISE_ERR_SIZE;
    }
    if (plaintext == NULL && plaintext_len != 0u) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (ad == NULL && ad_len != 0u) {
        return ESPHOME_NOISE_ERR_ARG;
    }

    nonce_bytes(cs->nonce, nonce);
    if (!noise_aead_encrypt(cs->key, nonce, ad, ad_len, plaintext, plaintext_len, out)) {
        return ESPHOME_NOISE_ERR_AUTH_FAILED;
    }
    /* A failed send is not a state change: the nonce advances only on success. */
    mutable_cs->nonce++;
    *out_len = plaintext_len + NOISE_CRYPTO_AEAD_TAG_BYTES;
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_decrypt(const esphome_noise_cipherstate_t *cs,
                                          const uint8_t *ad, size_t ad_len,
                                          const uint8_t *ciphertext, size_t ciphertext_len,
                                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t nonce[NOISE_CRYPTO_AEAD_NONCE_BYTES];
    esphome_noise_cipherstate_t *mutable_cs = (esphome_noise_cipherstate_t *)cs;
    size_t plaintext_len;

    if (cs == NULL || ciphertext == NULL || out_len == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    *out_len = 0u;
    if (!cs->has_key) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    if (ciphertext_len < NOISE_CRYPTO_AEAD_TAG_BYTES) {
        return ESPHOME_NOISE_ERR_SIZE;
    }
    if (cs->nonce == UINT64_MAX) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    plaintext_len = ciphertext_len - NOISE_CRYPTO_AEAD_TAG_BYTES;
    if (out_cap < plaintext_len) {
        return ESPHOME_NOISE_ERR_SIZE;
    }
    if (out == NULL && plaintext_len != 0u) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (ad == NULL && ad_len != 0u) {
        return ESPHOME_NOISE_ERR_ARG;
    }

    nonce_bytes(cs->nonce, nonce);
    if (!noise_aead_decrypt(cs->key, nonce, ad, ad_len, ciphertext, ciphertext_len, out)) {
        /* Section 5.1: on failure n is not incremented. */
        return ESPHOME_NOISE_ERR_AUTH_FAILED;
    }
    mutable_cs->nonce++;
    *out_len = plaintext_len;
    return ESPHOME_NOISE_OK;
}

esphome_noise_err_t esphome_noise_cipherstate_rekey(esphome_noise_cipherstate_t *cs)
{
    uint8_t zeros[NOISE_CRYPTO_AEAD_KEY_BYTES];
    uint8_t nonce[NOISE_CRYPTO_AEAD_NONCE_BYTES];
    uint8_t out[NOISE_CRYPTO_AEAD_KEY_BYTES + NOISE_CRYPTO_AEAD_TAG_BYTES];

    if (cs == NULL) {
        return ESPHOME_NOISE_ERR_ARG;
    }
    if (!cs->has_key) {
        return ESPHOME_NOISE_ERR_STATE;
    }
    memset(zeros, 0, sizeof(zeros));
    nonce_bytes(UINT64_MAX, nonce);
    if (!noise_aead_encrypt(cs->key, nonce, NULL, 0u, zeros, sizeof(zeros), out)) {
        return ESPHOME_NOISE_ERR_AUTH_FAILED;
    }
    memcpy(cs->key, out, NOISE_CRYPTO_AEAD_KEY_BYTES);
    noise_crypto_wipe(out, sizeof(out));
    noise_crypto_wipe(nonce, sizeof(nonce));
    noise_crypto_wipe(zeros, sizeof(zeros));
    return ESPHOME_NOISE_OK;
}

void esphome_noise_cipherstate_wipe(esphome_noise_cipherstate_t *cs)
{
    if (cs == NULL) {
        return;
    }
    noise_crypto_wipe(cs, sizeof(*cs));
}
