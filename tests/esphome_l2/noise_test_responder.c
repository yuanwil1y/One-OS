#include "noise_test_responder.h"

#include <string.h>

#include "esphome_noise_crypto.h"

/* HKDF(ck, ikm, n) as defined by the Noise specification (section 4.3). */
static void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len, uint8_t *out1,
                 uint8_t *out2, uint8_t *out3)
{
    noise_hkdf_sha256(ck, ikm, ikm_len, out1, out2, out3, out3 != NULL ? 3u : 2u);
}

static void mix_hash(ntr_state_t *st, const uint8_t *data, size_t len)
{
    noise_sha256_t sha;
    noise_sha256_init(&sha);
    noise_sha256_update(&sha, st->h, 32);
    if (len != 0u) {
        noise_sha256_update(&sha, data, len);
    }
    noise_sha256_final(&sha, st->h);
    noise_crypto_wipe(&sha, sizeof(sha));
}

/* MixKeyAndHash(psk): ck, temp_h, temp_k = HKDF(ck, psk, 3) */
static void mix_key_and_hash(ntr_state_t *st, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t new_ck[32];
    uint8_t temp_h[32];
    uint8_t temp_k[32];
    hkdf(st->ck, ikm, ikm_len, new_ck, temp_h, temp_k);
    memcpy(st->ck, new_ck, 32);
    mix_hash(st, temp_h, 32);
    memcpy(st->k, temp_k, 32);
    st->nonce = 0u;
    noise_crypto_wipe(new_ck, sizeof(new_ck));
    noise_crypto_wipe(temp_h, sizeof(temp_h));
    noise_crypto_wipe(temp_k, sizeof(temp_k));
}

/* MixKey(ikm): ck, temp_k = HKDF(ck, ikm, 2) */
static void mix_key(ntr_state_t *st, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t new_ck[32];
    uint8_t temp_k[32];
    hkdf(st->ck, ikm, ikm_len, new_ck, temp_k, NULL);
    memcpy(st->ck, new_ck, 32);
    memcpy(st->k, temp_k, 32);
    st->nonce = 0u;
    noise_crypto_wipe(new_ck, sizeof(new_ck));
    noise_crypto_wipe(temp_k, sizeof(temp_k));
}

static void nonce_bytes(uint64_t nonce, uint8_t out[12])
{
    memset(out, 0, 4u);
    for (unsigned i = 0; i < 8u; i++) {
        out[4u + i] = (uint8_t)((nonce >> (8u * i)) & 0xffu);
    }
}

void ntr_seeded_entropy(void *user, uint8_t *out, size_t len)
{
    uint32_t *counter = (uint32_t *)user;
    for (size_t i = 0; i < len; i++) {
        (*counter)++;
        out[i] = (uint8_t)((*counter * 2654435761u) >> 13);
    }
}

/* Host-test replacement for the ESP-IDF hardware RNG that esphome_api.c calls.
 * Deterministic on purpose: host runs have to be reproducible. This file is
 * test-only and is never compiled into firmware. */
static uint64_t g_stub_rng = 0x9e3779b97f4a7c15ull;

uint32_t esp_random(void)
{
    g_stub_rng ^= g_stub_rng << 13;
    g_stub_rng ^= g_stub_rng >> 7;
    g_stub_rng ^= g_stub_rng << 17;
    return (uint32_t)(g_stub_rng >> 32);
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        if ((i & 3u) == 0u) {
            g_stub_rng ^= g_stub_rng << 13;
            g_stub_rng ^= g_stub_rng >> 7;
            g_stub_rng ^= g_stub_rng << 17;
        }
        out[i] = (uint8_t)(g_stub_rng >> (8u * (i & 3u)));
    }
}

void ntr_reset(ntr_state_t *st, const uint8_t psk[32], const uint8_t prologue[12],
               const uint8_t e_priv[32], const uint8_t e_pub[32])
{
    static const char name[] = ESPHOME_NOISE_PROTOCOL_NAME;

    memset(st, 0, sizeof(*st));
    if (psk != NULL) {
        memcpy(st->psk, psk, 32);
    }
    noise_sha256(name, sizeof(name) - 1u, st->h);
    memcpy(st->ck, st->h, 32);
    /* MixHash(prologue), always, including a zero-length prologue. */
    mix_hash(st, prologue, 12);
    if (e_priv != NULL) {
        memcpy(st->e_priv, e_priv, 32);
    }
    noise_x25519_public_key(st->e_pub, st->e_priv);
    if (e_pub != NULL) {
        memcpy(st->e_pub, e_pub, 32);
    }
}

bool ntr_handshake(ntr_state_t *st, const uint8_t msg1[NTR_MSG_BYTES], uint8_t msg2[NTR_MSG_BYTES])
{
    uint8_t tag[16];
    uint8_t nonce[12];
    uint8_t ee[32];

    /* --- message 1: psk, e, payload --- */
    mix_key_and_hash(st, st->psk, 32);
    memcpy(st->re, msg1, 32);
    mix_hash(st, st->re, 32);
    mix_key(st, st->re, 32);
    nonce_bytes(st->nonce, nonce);
    if (!noise_aead_decrypt(st->k, nonce, st->h, 32, msg1 + 32, 16, tag)) {
        st->handshake_failed = true;
        return false;
    }
    st->nonce++;
    mix_hash(st, msg1 + 32, 16);

    /* --- message 2: e, ee, payload --- */
    memcpy(msg2, st->e_pub, 32);
    mix_hash(st, st->e_pub, 32);
    mix_key(st, st->e_pub, 32);

    noise_x25519(ee, st->e_priv, st->re);
    mix_key(st, ee, 32);
    noise_crypto_wipe(ee, sizeof(ee));

    nonce_bytes(st->nonce, nonce);
    if (!noise_aead_encrypt(st->k, nonce, st->h, 32, NULL, 0u, tag)) {
        st->handshake_failed = true;
        return false;
    }
    memcpy(msg2 + 32, tag, 16);
    st->nonce++;
    mix_hash(st, tag, 16);

    st->ready = true;
    return true;
}

bool ntr_split(ntr_state_t *st, uint8_t server_send[32], uint8_t server_recv[32])
{
    if (!st->ready) {
        return false;
    }
    hkdf(st->ck, NULL, 0u, server_recv, server_send, NULL);
    st->split_done = true;
    return true;
}

size_t ntr_seal(ntr_state_t *st, uint16_t type, const uint8_t *payload, size_t len, uint8_t *out,
                size_t cap)
{
    uint8_t pt[4 + NTR_MAX_FRAME];
    esphome_noise_cipherstate_t cs;
    size_t enc_len = 0;

    if (!st->split_done || len > sizeof(pt) - 4u || cap < 3u) {
        return 0u;
    }
    pt[0] = (uint8_t)(type >> 8);
    pt[1] = (uint8_t)(type & 0xffu);
    pt[2] = (uint8_t)(len >> 8);
    pt[3] = (uint8_t)(len & 0xffu);
    if (len != 0u) {
        memcpy(pt + 4, payload, len);
    }
    if (esphome_noise_cipherstate_init(&cs, st->send_key) != ESPHOME_NOISE_OK) {
        return 0u;
    }
    cs.nonce = st->nonce;
    if (esphome_noise_encrypt(&cs, NULL, 0u, pt, 4u + len, out + 3, cap - 3u, &enc_len) !=
        ESPHOME_NOISE_OK) {
        return 0u;
    }
    st->nonce = cs.nonce;
    out[0] = ESPHOME_NOISE_PREAMBLE;
    out[1] = (uint8_t)(enc_len >> 8);
    out[2] = (uint8_t)(enc_len & 0xffu);
    return 3u + enc_len;
}

bool ntr_open(ntr_state_t *st, const uint8_t *frame, size_t frame_len, uint16_t *type,
              uint8_t *payload, size_t cap, size_t *payload_len)
{
    uint8_t pt[4 + NTR_MAX_FRAME];
    esphome_noise_cipherstate_t cs;
    size_t plain_len = 0;

    if (!st->split_done || frame_len < 16u) {
        return false;
    }
    if (esphome_noise_cipherstate_init(&cs, st->recv_key) != ESPHOME_NOISE_OK) {
        return false;
    }
    cs.nonce = st->nonce;
    if (esphome_noise_decrypt(&cs, NULL, 0u, frame, frame_len, pt, sizeof(pt), &plain_len) !=
        ESPHOME_NOISE_OK) {
        return false;
    }
    st->nonce = cs.nonce;
    if (plain_len < 4u) {
        return false;
    }
    *type = (uint16_t)(((uint16_t)pt[0] << 8) | pt[1]);
    *payload_len = plain_len - 4u;
    if (*payload_len > cap) {
        return false;
    }
    memcpy(payload, pt + 4, *payload_len);
    return true;
}
