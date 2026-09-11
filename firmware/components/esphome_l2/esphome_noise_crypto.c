/*
 * esphome_noise_crypto.c — portable C11 cryptographic primitives for the
 * ESPHome native-API Noise handshake.  See esphome_noise_crypto.h.
 *
 * Design / representation notes
 * =============================
 *  * GF(2^255-19) uses TEN signed 32-bit limbs in radix 2^25.5: limb k has
 *    weight 51*(k/2) + 26*(k & 1), so even limbs carry 26 bits and odd limbs
 *    25 bits (255 bits in total).  This is the classic "ref10" layout.  All
 *    products are accumulated in int64_t, so the field arithmetic needs no
 *    integer type wider than 64 bits.
 *
 *    The 5x51-bit layout was rejected on purpose: without __int128 a 51-bit
 *    limb product has to be split into 32-bit halves by hand (or done in a
 *    10x25.5-bit form anyway), which is more code and more ways to be wrong.
 *    With the 10-limb layout the schoolbook product is a plain 10x10 loop whose
 *    only bookkeeping is "fold 2^255 -> 19 when i+j >= 10" and "double when
 *    both limbs are odd"; both facts were derived from the limb weights and
 *    are checked by the RFC 7748 test vectors.  Everything stays inside
 *    int64_t, so the module is trivially portable to 32-bit RISC-V.
 *
 *  * unsigned __int128 is deliberately NOT used anywhere in this file: the
 *    module must compile unchanged with clang/gcc on the host and with the
 *    ESP-IDF RISC-V toolchain, and 128-bit arithmetic would also make the
 *    constant-time reasoning harder on a 32-bit target.
 *
 *  * fe_sar() implements an arithmetic right shift using only unsigned
 *    arithmetic, so no code here relies on the (implementation-defined)
 *    behaviour of >> on negative signed values.  Signed integer overflow,
 *    out-of-range shifts, unaligned accesses and strict-aliasing violations do
 *    not occur; the only implementation-defined operation relied upon is the
 *    conversion of an unsigned value above INT32_MAX to int32_t in fe_cswap(),
 *    which is the standard two's-complement wrap and is what every compiler
 *    targeted here (clang, gcc, RISC-V GCC) does.
 *
 *  * No heap allocation, no global mutable state, no recursion, no OS or
 *    network headers; every buffer is caller supplied, every function is
 *    reentrant.
 *
 *  * The X25519 Montgomery ladder performs a fixed number of iterations and
 *    swaps with fe_cswap(); the Poly1305 tag comparison accumulates the
 *    difference of all 16 bytes and compares once, never memcmp().
 */

#include "esphome_noise_crypto.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Sensitive wipe                                                            */
/* ------------------------------------------------------------------------- */

void noise_crypto_wipe(void *data, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (len > 0u) {
        *p++ = 0u;
        len--;
    }
}

/* ------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4)                                                      */
/* ------------------------------------------------------------------------- */

static const uint32_t noise_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t noise_rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void noise_sha256_compress(uint32_t state[8], const uint8_t block[NOISE_CRYPTO_SHA256_BLOCK])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)block[4u * i] << 24) | ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) | (uint32_t)block[4u * i + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = noise_rotr32(w[i - 15u], 7u) ^ noise_rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = noise_rotr32(w[i - 2u], 17u) ^ noise_rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0u; i < 64u; i++) {
        uint32_t big_s1 = noise_rotr32(e, 6u) ^ noise_rotr32(e, 11u) ^ noise_rotr32(e, 25u);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + big_s1 + ch + noise_sha256_k[i] + w[i];
        uint32_t big_s0 = noise_rotr32(a, 2u) ^ noise_rotr32(a, 13u) ^ noise_rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = big_s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;

    noise_crypto_wipe(w, sizeof w);
}

void noise_sha256_init(noise_sha256_t *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0u;
    ctx->buflen = 0u;
}

void noise_sha256_update(noise_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (len == 0u) {
        return; /* data may be NULL in this case */
    }

    ctx->bitlen += (uint64_t)len << 3;

    if (ctx->buflen > 0u) {
        size_t take = NOISE_CRYPTO_SHA256_BLOCK - ctx->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == NOISE_CRYPTO_SHA256_BLOCK) {
            noise_sha256_compress(ctx->state, ctx->buf);
            ctx->buflen = 0u;
        }
    }

    while (len >= NOISE_CRYPTO_SHA256_BLOCK) {
        noise_sha256_compress(ctx->state, p);
        p += NOISE_CRYPTO_SHA256_BLOCK;
        len -= NOISE_CRYPTO_SHA256_BLOCK;
    }

    if (len > 0u) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void noise_sha256_final(noise_sha256_t *ctx, uint8_t out[NOISE_CRYPTO_SHA256_BYTES])
{
    uint64_t bitlen = ctx->bitlen;
    size_t i = ctx->buflen;
    unsigned j;

    ctx->buf[i++] = 0x80u;
    if (i > 56u) {
        memset(ctx->buf + i, 0, NOISE_CRYPTO_SHA256_BLOCK - i);
        noise_sha256_compress(ctx->state, ctx->buf);
        i = 0u;
    }
    memset(ctx->buf + i, 0, 56u - i);
    for (j = 0u; j < 8u; j++) {
        ctx->buf[56u + j] = (uint8_t)(bitlen >> (56u - 8u * j));
    }
    noise_sha256_compress(ctx->state, ctx->buf);

    for (j = 0u; j < 8u; j++) {
        out[4u * j] = (uint8_t)(ctx->state[j] >> 24);
        out[4u * j + 1u] = (uint8_t)(ctx->state[j] >> 16);
        out[4u * j + 2u] = (uint8_t)(ctx->state[j] >> 8);
        out[4u * j + 3u] = (uint8_t)ctx->state[j];
    }

    /* The context is single use: leave nothing sensitive behind. */
    noise_crypto_wipe(ctx, sizeof *ctx);
}

void noise_sha256(const void *data, size_t len, uint8_t out[NOISE_CRYPTO_SHA256_BYTES])
{
    noise_sha256_t ctx;

    noise_sha256_init(&ctx);
    noise_sha256_update(&ctx, data, len);
    noise_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------------- */
/* HMAC-SHA256 (RFC 2104)                                                    */
/* ------------------------------------------------------------------------- */

void noise_hmac_sha256_init(noise_hmac_sha256_t *ctx, const uint8_t *key, size_t key_len)
{
    uint8_t block[NOISE_CRYPTO_SHA256_BLOCK];
    uint8_t hashed[NOISE_CRYPTO_SHA256_BYTES];
    unsigned i;

    memset(block, 0, sizeof block);

    if (key_len > NOISE_CRYPTO_SHA256_BLOCK) {
        noise_sha256(key, key_len, hashed);
        memcpy(block, hashed, sizeof hashed);
        noise_crypto_wipe(hashed, sizeof hashed);
    } else if (key_len > 0u) {
        memcpy(block, key, key_len);
    }

    for (i = 0u; i < NOISE_CRYPTO_SHA256_BLOCK; i++) {
        ctx->opad[i] = (uint8_t)(block[i] ^ 0x5cu);
        block[i] ^= 0x36u; /* ipad, in place */
    }

    noise_sha256_init(&ctx->inner);
    noise_sha256_update(&ctx->inner, block, sizeof block);
    noise_crypto_wipe(block, sizeof block);
}

void noise_hmac_sha256_update(noise_hmac_sha256_t *ctx, const void *data, size_t len)
{
    noise_sha256_update(&ctx->inner, data, len);
}

void noise_hmac_sha256_final(noise_hmac_sha256_t *ctx, uint8_t out[NOISE_CRYPTO_SHA256_BYTES])
{
    uint8_t inner_digest[NOISE_CRYPTO_SHA256_BYTES];

    noise_sha256_final(&ctx->inner, inner_digest);
    noise_sha256_init(&ctx->inner);
    noise_sha256_update(&ctx->inner, ctx->opad, NOISE_CRYPTO_SHA256_BLOCK);
    noise_sha256_update(&ctx->inner, inner_digest, sizeof inner_digest);
    noise_sha256_final(&ctx->inner, out);

    noise_crypto_wipe(inner_digest, sizeof inner_digest);
    noise_crypto_wipe(ctx, sizeof *ctx);
}

void noise_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
                       uint8_t out[NOISE_CRYPTO_SHA256_BYTES])
{
    noise_hmac_sha256_t ctx;

    noise_hmac_sha256_init(&ctx, key, key_len);
    noise_hmac_sha256_update(&ctx, data, len);
    noise_hmac_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------------- */
/* HKDF (RFC 5869) with SHA-256, Noise framing                               */
/* ------------------------------------------------------------------------- */

void noise_hkdf_sha256(const uint8_t *chaining_key, const uint8_t *ikm, size_t ikm_len,
                       uint8_t *out1, uint8_t *out2, uint8_t *out3, unsigned num_outputs)
{
    uint8_t temp_key[NOISE_CRYPTO_SHA256_BYTES];
    uint8_t buf[NOISE_CRYPTO_SHA256_BYTES + 1u];

    /* temp_key = HMAC(chaining_key, ikm) */
    noise_hmac_sha256(chaining_key, NOISE_CRYPTO_SHA256_BYTES, ikm, ikm_len, temp_key);

    /* out1 = HMAC(temp_key, 0x01) */
    buf[0] = 0x01u;
    noise_hmac_sha256(temp_key, sizeof temp_key, buf, 1u, out1);

    if (num_outputs >= 2u) {
        /* out2 = HMAC(temp_key, out1 || 0x02) */
        memcpy(buf, out1, NOISE_CRYPTO_SHA256_BYTES);
        buf[NOISE_CRYPTO_SHA256_BYTES] = 0x02u;
        noise_hmac_sha256(temp_key, sizeof temp_key, buf, sizeof buf, out2);
    }

    if (num_outputs >= 3u) {
        /* out3 = HMAC(temp_key, out2 || 0x03) */
        memcpy(buf, out2, NOISE_CRYPTO_SHA256_BYTES);
        buf[NOISE_CRYPTO_SHA256_BYTES] = 0x03u;
        noise_hmac_sha256(temp_key, sizeof temp_key, buf, sizeof buf, out3);
    }

    noise_crypto_wipe(temp_key, sizeof temp_key);
    noise_crypto_wipe(buf, sizeof buf);
}

/* ------------------------------------------------------------------------- */
/* Little-endian loads/stores (byte-wise: no alignment or aliasing concerns)  */
/* ------------------------------------------------------------------------- */

static uint32_t noise_load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void noise_store32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void noise_store64_le(uint8_t *p, uint64_t v)
{
    unsigned i;
    for (i = 0u; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* ------------------------------------------------------------------------- */
/* ChaCha20 (RFC 8439 section 2.3)                                           */
/* ------------------------------------------------------------------------- */

static uint32_t noise_rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static void noise_chacha20_quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b; *d ^= *a; *d = noise_rotl32(*d, 16);
    *c += *d; *b ^= *c; *b = noise_rotl32(*b, 12);
    *a += *b; *d ^= *a; *d = noise_rotl32(*d, 8);
    *c += *d; *b ^= *c; *b = noise_rotl32(*b, 7);
}

/* Produces the 16 little-endian words of one ChaCha20 block (counter is the
   32-bit block counter; the nonce is 96 bits). */
static void noise_chacha20_block(uint32_t out[16], const uint8_t key[32], const uint8_t nonce[12],
                                 uint32_t counter)
{
    uint32_t x[16];
    unsigned i;

    x[0] = 0x61707865u; x[1] = 0x3320646eu; x[2] = 0x79622d32u; x[3] = 0x6b206574u;
    for (i = 0u; i < 8u; i++) {
        x[4u + i] = noise_load32_le(key + 4u * i);
    }
    x[12] = counter;
    x[13] = noise_load32_le(nonce);
    x[14] = noise_load32_le(nonce + 4u);
    x[15] = noise_load32_le(nonce + 8u);

    for (i = 0u; i < 16u; i++) {
        out[i] = x[i];
    }

    for (i = 0u; i < 10u; i++) {
        /* column rounds */
        noise_chacha20_quarter_round(&x[0], &x[4], &x[8], &x[12]);
        noise_chacha20_quarter_round(&x[1], &x[5], &x[9], &x[13]);
        noise_chacha20_quarter_round(&x[2], &x[6], &x[10], &x[14]);
        noise_chacha20_quarter_round(&x[3], &x[7], &x[11], &x[15]);
        /* diagonal rounds */
        noise_chacha20_quarter_round(&x[0], &x[5], &x[10], &x[15]);
        noise_chacha20_quarter_round(&x[1], &x[6], &x[11], &x[12]);
        noise_chacha20_quarter_round(&x[2], &x[7], &x[8], &x[13]);
        noise_chacha20_quarter_round(&x[3], &x[4], &x[9], &x[14]);
    }

    for (i = 0u; i < 16u; i++) {
        out[i] += x[i];
    }

    noise_crypto_wipe(x, sizeof x);
}

/* out = in ^ keystream (in == NULL is allowed when len == 0) */
static void noise_chacha20_xor(uint8_t *out, const uint8_t *in, size_t len,
                               const uint8_t key[32], const uint8_t nonce[12], uint32_t counter)
{
    uint32_t block[16];
    size_t off = 0u;

    while (off < len) {
        size_t n = len - off;
        size_t i;
        if (n > 64u) {
            n = 64u;
        }
        noise_chacha20_block(block, key, nonce, counter);
        counter++;
        for (i = 0u; i < n; i++) {
            uint8_t ks = (uint8_t)(block[i >> 2] >> (8u * (i & 3u)));
            out[off + i] = (uint8_t)(in[off + i] ^ ks);
        }
        off += n;
    }

    noise_crypto_wipe(block, sizeof block);
}

/* Keystream-only variant, used to derive the Poly1305 one-time key. */
static void noise_chacha20_keystream(uint8_t *out, size_t len, const uint8_t key[32],
                                     const uint8_t nonce[12], uint32_t counter)
{
    uint32_t block[16];
    size_t off = 0u;

    while (off < len) {
        size_t n = len - off;
        size_t i;
        if (n > 64u) {
            n = 64u;
        }
        noise_chacha20_block(block, key, nonce, counter);
        counter++;
        for (i = 0u; i < n; i++) {
            out[off + i] = (uint8_t)(block[i >> 2] >> (8u * (i & 3u)));
        }
        off += n;
    }

    noise_crypto_wipe(block, sizeof block);
}

/* ------------------------------------------------------------------------- */
/* Poly1305 (RFC 8439 section 2.5), 5 x 26-bit limbs, uint64 accumulators     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    uint8_t buf[16];
} noise_poly1305_t;

static void noise_poly1305_blocks(noise_poly1305_t *ctx, const uint8_t *m, size_t bytes, uint32_t hibit)
{
    const uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    const uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t c;

    while (bytes >= 16u) {
        uint64_t d0, d1, d2, d3, d4;

        h0 += noise_load32_le(m) & 0x3ffffffu;
        h1 += (noise_load32_le(m + 3u) >> 2) & 0x3ffffffu;
        h2 += (noise_load32_le(m + 6u) >> 4) & 0x3ffffffu;
        h3 += (noise_load32_le(m + 9u) >> 6) & 0x3ffffffu;
        h4 += (noise_load32_le(m + 12u) >> 8) | hibit;

        d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu; h0 += c * 5u;
        c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

        m += 16u;
        bytes -= 16u;
    }

    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

static void noise_poly1305_init(noise_poly1305_t *ctx, const uint8_t key[32])
{
    /* r, clamped: r &= 0x0ffffffc0ffffffc0ffffffc0fffffff */
    ctx->r[0] = noise_load32_le(key) & 0x3ffffffu;
    ctx->r[1] = (noise_load32_le(key + 3u) >> 2) & 0x3ffff03u;
    ctx->r[2] = (noise_load32_le(key + 6u) >> 4) & 0x3ffc0ffu;
    ctx->r[3] = (noise_load32_le(key + 9u) >> 6) & 0x3f03fffu;
    ctx->r[4] = (noise_load32_le(key + 12u) >> 8) & 0x00fffffu;

    ctx->h[0] = 0u; ctx->h[1] = 0u; ctx->h[2] = 0u; ctx->h[3] = 0u; ctx->h[4] = 0u;

    ctx->pad[0] = noise_load32_le(key + 16u);
    ctx->pad[1] = noise_load32_le(key + 20u);
    ctx->pad[2] = noise_load32_le(key + 24u);
    ctx->pad[3] = noise_load32_le(key + 28u);

    ctx->leftover = 0u;
    memset(ctx->buf, 0, sizeof ctx->buf);
}

static void noise_poly1305_update(noise_poly1305_t *ctx, const uint8_t *m, size_t bytes)
{
    if (bytes == 0u) {
        return; /* m may be NULL in this case */
    }

    if (ctx->leftover > 0u) {
        size_t want = 16u - ctx->leftover;
        if (want > bytes) {
            want = bytes;
        }
        memcpy(ctx->buf + ctx->leftover, m, want);
        ctx->leftover += want;
        m += want;
        bytes -= want;
        if (ctx->leftover == 16u) {
            noise_poly1305_blocks(ctx, ctx->buf, 16u, 1u << 24);
            ctx->leftover = 0u;
        }
    }

    if (bytes >= 16u) {
        size_t full = bytes & ~(size_t)15u;
        noise_poly1305_blocks(ctx, m, full, 1u << 24);
        m += full;
        bytes -= full;
    }

    if (bytes > 0u) {
        memcpy(ctx->buf, m, bytes);
        ctx->leftover = bytes;
    }
}

static void noise_poly1305_final(noise_poly1305_t *ctx, uint8_t tag[16])
{
    uint32_t h0, h1, h2, h3, h4, c;
    uint32_t g0, g1, g2, g3, g4, mask;
    uint64_t f;

    if (ctx->leftover > 0u) {
        size_t i = ctx->leftover;
        ctx->buf[i++] = 1u;
        while (i < 16u) {
            ctx->buf[i++] = 0u;
        }
        ctx->leftover = 0u;
        noise_poly1305_blocks(ctx, ctx->buf, 16u, 0u); /* no 2^128 bit on the last block */
    }

    h0 = ctx->h[0]; h1 = ctx->h[1]; h2 = ctx->h[2]; h3 = ctx->h[3]; h4 = ctx->h[4];

    /* fully carry h */
    c = h1 >> 26; h1 &= 0x3ffffffu; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffffu; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffffu; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffffu; h0 += c * 5u;
    c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

    /* compute h + -p */
    g0 = h0 + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
    g1 = h1 + c;  c = g1 >> 26; g1 &= 0x3ffffffu;
    g2 = h2 + c;  c = g2 >> 26; g2 &= 0x3ffffffu;
    g3 = h3 + c;  c = g3 >> 26; g3 &= 0x3ffffffu;
    g4 = h4 + c - (1u << 26);

    /* select h if h < p, or h + -p if h >= p (constant time) */
    mask = (g4 >> 31) - 1u; /* 0xffffffff when g4 wrapped (h < p) */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* h = h % 2^128 */
    h0 = (h0 | (h1 << 26)) & 0xffffffffu;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;

    /* tag = (h + pad) % 2^128 */
    f = (uint64_t)h0 + ctx->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32); h3 = (uint32_t)f;

    noise_store32_le(tag, h0);
    noise_store32_le(tag + 4u, h1);
    noise_store32_le(tag + 8u, h2);
    noise_store32_le(tag + 12u, h3);

    noise_crypto_wipe(ctx, sizeof *ctx);
}

/* Constant-time comparison of two 16-byte tags: no early exit, no memcmp. */
static bool noise_tag_equal(const uint8_t *a, const uint8_t *b)
{
    uint32_t diff = 0u;
    unsigned i;
    for (i = 0u; i < 16u; i++) {
        diff |= (uint32_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}

/* ------------------------------------------------------------------------- */
/* ChaCha20-Poly1305 AEAD (RFC 8439 section 2.8)                             */
/* ------------------------------------------------------------------------- */

static void noise_aead_mac(uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *ad, size_t ad_len, const uint8_t *ct, size_t ct_len)
{
    noise_poly1305_t poly;
    uint8_t otk[64];
    uint8_t lens[16];
    static const uint8_t zeros[16] = { 0u };

    noise_chacha20_keystream(otk, sizeof otk, key, nonce, 0u);
    noise_poly1305_init(&poly, otk);

    noise_poly1305_update(&poly, ad, ad_len);
    if ((ad_len & 15u) != 0u) {
        noise_poly1305_update(&poly, zeros, 16u - (ad_len & 15u));
    }

    noise_poly1305_update(&poly, ct, ct_len);
    if ((ct_len & 15u) != 0u) {
        noise_poly1305_update(&poly, zeros, 16u - (ct_len & 15u));
    }

    noise_store64_le(lens, (uint64_t)ad_len);
    noise_store64_le(lens + 8u, (uint64_t)ct_len);
    noise_poly1305_update(&poly, lens, sizeof lens);

    noise_poly1305_final(&poly, tag);

    noise_crypto_wipe(otk, sizeof otk);
    noise_crypto_wipe(&poly, sizeof poly);
}

bool noise_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                        const uint8_t *plaintext, size_t plaintext_len, uint8_t *out)
{
    uint8_t tag[16];

    noise_chacha20_xor(out, plaintext, plaintext_len, key, nonce, 1u);
    noise_aead_mac(tag, key, nonce, ad, ad_len, out, plaintext_len);
    memcpy(out + plaintext_len, tag, sizeof tag);

    noise_crypto_wipe(tag, sizeof tag);
    return true;
}

bool noise_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                        const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *out)
{
    uint8_t tag[16];
    size_t ct_len;

    if (ciphertext_len < NOISE_CRYPTO_AEAD_TAG_BYTES) {
        return false;
    }
    ct_len = ciphertext_len - NOISE_CRYPTO_AEAD_TAG_BYTES;

    noise_aead_mac(tag, key, nonce, ad, ad_len, ciphertext, ct_len);
    if (!noise_tag_equal(tag, ciphertext + ct_len)) {
        noise_crypto_wipe(tag, sizeof tag);
        /* Never expose unauthenticated plaintext. */
        noise_crypto_wipe(out, ct_len);
        return false;
    }
    noise_crypto_wipe(tag, sizeof tag);

    noise_chacha20_xor(out, ciphertext, ct_len, key, nonce, 1u);
    return true;
}

/* ------------------------------------------------------------------------- */
/* X25519 (RFC 7748)                                                         */
/* ------------------------------------------------------------------------- */

/* Field element: ten limbs, limb k has weight 51*(k/2) + 26*(k & 1). */
typedef int32_t noise_fe[10];

/* Arithmetic right shift of a signed 64-bit value, defined for negative x and
   portable (no reliance on implementation-defined >> of negative values). */
static int64_t noise_fe_sar(int64_t x, unsigned n)
{
    uint64_t u = (uint64_t)x;
    uint64_t sign = (uint64_t)0 - (u >> 63); /* all ones when x < 0 */
    return (int64_t)((u >> n) | (sign << (64u - n)));
}

/* c * 2^n for a signed c.  Written as a multiplication on purpose: shifting a
   negative signed value left is undefined behaviour in C11 (UBSan flags it),
   while the multiplication is well defined and cannot overflow here because
   |c| <= 2^2 and n <= 26 in every carry chain below. */
static int64_t noise_fe_shl(int64_t c, unsigned n)
{
    return c * (((int64_t)1) << n);
}

/* Extract nbits (<= 26) little-endian bits starting at bit position bitoff
   (bitoff + nbits <= 255). */
static uint64_t noise_load_bits(const uint8_t *s, unsigned bitoff, unsigned nbits)
{
    uint64_t v = 0u;
    unsigned base = bitoff >> 3;
    unsigned i;

    for (i = 0u; i < 8u; i++) {
        unsigned idx = base + i;
        if (idx >= 32u) {
            break;
        }
        v |= (uint64_t)s[idx] << (8u * i);
    }
    v >>= (bitoff & 7u);
    return v & ((((uint64_t)1u) << nbits) - 1u);
}

static unsigned noise_fe_bits(int i)
{
    return 26u - (unsigned)(i & 1); /* 26 bits on even limbs, 25 on odd limbs */
}

static unsigned noise_fe_weight(int i)
{
    return 51u * (unsigned)(i / 2) + 26u * (unsigned)(i & 1);
}

/* h = s mod 2^255 (the top bit of s is ignored, as RFC 7748 requires) */
static void noise_fe_frombytes(noise_fe h, const uint8_t s[32])
{
    int i;
    for (i = 0; i < 10; i++) {
        h[i] = (int32_t)noise_load_bits(s, noise_fe_weight(i), noise_fe_bits(i));
    }
}

/* s = h mod 2^255 - 19, canonical little-endian encoding */
static void noise_fe_tobytes(uint8_t s[32], const noise_fe h)
{
    int64_t t[10];
    int64_t q, c;
    int i;

    for (i = 0; i < 10; i++) {
        t[i] = h[i];
    }

    /* q = floor(h / 2^255) as a small signed integer (see the preconditions on
       the bounds of the limbs, |h| <= 1.1*2^25 on even and 1.1*2^24 on odd
       limbs, which is the postcondition of noise_fe_mul). */
    q = noise_fe_sar(19 * t[9] + (((int64_t)1) << 24), 25);
    for (i = 0; i < 10; i++) {
        q = noise_fe_sar(t[i] + q, noise_fe_bits(i));
    }
    /* output h - (2^255 - 19) * q, which lies in [0, 2^255 - 20) */
    t[0] += 19 * q;

    /* carry the result, dropping the carry out of limb 9 (i.e. mod 2^255) */
    for (i = 0; i < 10; i++) {
        unsigned sh = noise_fe_bits(i);
        c = noise_fe_sar(t[i], sh);
        t[i] -= noise_fe_shl(c, sh);
        if (i < 9) {
            t[i + 1] += c;
        }
    }

    memset(s, 0, 32);
    for (i = 0; i < 10; i++) {
        uint64_t v = (uint64_t)t[i];
        unsigned bitoff = noise_fe_weight(i);
        unsigned nbits = noise_fe_bits(i);
        unsigned b;
        for (b = 0u; b < nbits; b++) {
            if (((v >> b) & 1u) != 0u) {
                s[(bitoff + b) >> 3] |= (uint8_t)(1u << ((bitoff + b) & 7u));
            }
        }
    }
}

/* h = f * g mod 2^255 - 19.  Preconditions: |f|, |g| <= 1.1*2^26 on even and
   1.1*2^25 on odd limbs (which is what the add/sub/mul postconditions give).
   The product of limb i and limb j has weight w(i)+w(j); when i+j >= 10 it is
   reduced with 2^255 == 19, and when both limbs are odd the weight sum is one
   bit above the target limb, hence the extra factor of two. */
static void noise_fe_mul(noise_fe h, const noise_fe f, const noise_fe g)
{
    int64_t t[10];
    int i, j, k;

    for (k = 0; k < 10; k++) {
        t[k] = 0;
    }

    for (i = 0; i < 10; i++) {
        int64_t fi = f[i];
        for (j = 0; j < 10; j++) {
            int64_t factor = ((i & 1) != 0 && (j & 1) != 0) ? 2 : 1;
            int idx = i + j;
            if (idx >= 10) {
                idx -= 10;
                factor *= 19;
            }
            t[idx] += fi * (int64_t)g[j] * factor;
        }
    }

    /* carry chain (ref10 order) */
    {
        int64_t c;
        c = noise_fe_sar(t[0] + (((int64_t)1) << 25), 26); t[1] += c; t[0] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[4] + (((int64_t)1) << 25), 26); t[5] += c; t[4] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[1] + (((int64_t)1) << 24), 25); t[2] += c; t[1] -= noise_fe_shl(c, 25);
        c = noise_fe_sar(t[5] + (((int64_t)1) << 24), 25); t[6] += c; t[5] -= noise_fe_shl(c, 25);
        c = noise_fe_sar(t[2] + (((int64_t)1) << 25), 26); t[3] += c; t[2] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[6] + (((int64_t)1) << 25), 26); t[7] += c; t[6] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[3] + (((int64_t)1) << 24), 25); t[4] += c; t[3] -= noise_fe_shl(c, 25);
        c = noise_fe_sar(t[7] + (((int64_t)1) << 24), 25); t[8] += c; t[7] -= noise_fe_shl(c, 25);
        c = noise_fe_sar(t[4] + (((int64_t)1) << 25), 26); t[5] += c; t[4] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[8] + (((int64_t)1) << 25), 26); t[9] += c; t[8] -= noise_fe_shl(c, 26);
        c = noise_fe_sar(t[9] + (((int64_t)1) << 24), 25); t[0] += c * 19; t[9] -= noise_fe_shl(c, 25);
        c = noise_fe_sar(t[0] + (((int64_t)1) << 25), 26); t[1] += c; t[0] -= noise_fe_shl(c, 26);
    }

    for (k = 0; k < 10; k++) {
        h[k] = (int32_t)t[k];
    }
}

static void noise_fe_sq(noise_fe h, const noise_fe f)
{
    noise_fe_mul(h, f, f);
}

static void noise_fe_add(noise_fe h, const noise_fe f, const noise_fe g)
{
    int i;
    for (i = 0; i < 10; i++) {
        h[i] = f[i] + g[i];
    }
}

static void noise_fe_sub(noise_fe h, const noise_fe f, const noise_fe g)
{
    int i;
    for (i = 0; i < 10; i++) {
        h[i] = f[i] - g[i];
    }
}

static void noise_fe_copy(noise_fe h, const noise_fe f)
{
    int i;
    for (i = 0; i < 10; i++) {
        h[i] = f[i];
    }
}

static void noise_fe_0(noise_fe h)
{
    int i;
    for (i = 0; i < 10; i++) {
        h[i] = 0;
    }
}

static void noise_fe_1(noise_fe h)
{
    int i;
    h[0] = 1;
    for (i = 1; i < 10; i++) {
        h[i] = 0;
    }
}

/* Constant-time conditional swap: b must be 0 or 1. */
static void noise_fe_cswap(noise_fe f, noise_fe g, int32_t b)
{
    int32_t mask = -b; /* 0 or -1 */
    int i;
    for (i = 0; i < 10; i++) {
        int32_t x = (f[i] ^ g[i]) & mask;
        f[i] ^= x;
        g[i] ^= x;
    }
}

/* h = f * 121666 */
static void noise_fe_mul121666(noise_fe h, const noise_fe f)
{
    int64_t t[10];
    int64_t c;
    int i;

    for (i = 0; i < 10; i++) {
        t[i] = (int64_t)f[i] * 121666;
    }

    c = noise_fe_sar(t[0] + (((int64_t)1) << 25), 26); t[1] += c; t[0] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[4] + (((int64_t)1) << 25), 26); t[5] += c; t[4] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[1] + (((int64_t)1) << 24), 25); t[2] += c; t[1] -= noise_fe_shl(c, 25);
    c = noise_fe_sar(t[5] + (((int64_t)1) << 24), 25); t[6] += c; t[5] -= noise_fe_shl(c, 25);
    c = noise_fe_sar(t[2] + (((int64_t)1) << 25), 26); t[3] += c; t[2] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[6] + (((int64_t)1) << 25), 26); t[7] += c; t[6] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[3] + (((int64_t)1) << 24), 25); t[4] += c; t[3] -= noise_fe_shl(c, 25);
    c = noise_fe_sar(t[7] + (((int64_t)1) << 24), 25); t[8] += c; t[7] -= noise_fe_shl(c, 25);
    c = noise_fe_sar(t[4] + (((int64_t)1) << 25), 26); t[5] += c; t[4] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[8] + (((int64_t)1) << 25), 26); t[9] += c; t[8] -= noise_fe_shl(c, 26);
    c = noise_fe_sar(t[9] + (((int64_t)1) << 24), 25); t[0] += c * 19; t[9] -= noise_fe_shl(c, 25);
    c = noise_fe_sar(t[0] + (((int64_t)1) << 25), 26); t[1] += c; t[0] -= noise_fe_shl(c, 26);

    for (i = 0; i < 10; i++) {
        h[i] = (int32_t)t[i];
    }
}

/* out = z^(2^255 - 21) = z^-1 (addition chain, ref10 shape) */
static void noise_fe_invert(noise_fe out, const noise_fe z)
{
    noise_fe t0, t1, t2, t3;
    int i;

    noise_fe_sq(t0, z);                                  /* 2 */
    noise_fe_sq(t1, t0);
    noise_fe_sq(t1, t1);                                 /* 8 */
    noise_fe_mul(t1, z, t1);                             /* 9 */
    noise_fe_mul(t0, t0, t1);                            /* 11 */
    noise_fe_sq(t2, t0);                                 /* 22 */
    noise_fe_mul(t1, t1, t2);                            /* 2^5 - 1 */
    noise_fe_sq(t2, t1);
    for (i = 1; i < 5; i++) {
        noise_fe_sq(t2, t2);
    }
    noise_fe_mul(t1, t2, t1);                            /* 2^10 - 1 */
    noise_fe_sq(t2, t1);
    for (i = 1; i < 10; i++) {
        noise_fe_sq(t2, t2);
    }
    noise_fe_mul(t2, t2, t1);                            /* 2^20 - 1 */
    noise_fe_sq(t3, t2);
    for (i = 1; i < 20; i++) {
        noise_fe_sq(t3, t3);
    }
    noise_fe_mul(t2, t3, t2);                            /* 2^40 - 1 */
    noise_fe_sq(t2, t2);
    for (i = 1; i < 10; i++) {
        noise_fe_sq(t2, t2);
    }
    noise_fe_mul(t1, t2, t1);                            /* 2^50 - 1 */
    noise_fe_sq(t2, t1);
    for (i = 1; i < 50; i++) {
        noise_fe_sq(t2, t2);
    }
    noise_fe_mul(t2, t2, t1);                            /* 2^100 - 1 */
    noise_fe_sq(t3, t2);
    for (i = 1; i < 100; i++) {
        noise_fe_sq(t3, t3);
    }
    noise_fe_mul(t2, t3, t2);                            /* 2^200 - 1 */
    noise_fe_sq(t2, t2);
    for (i = 1; i < 50; i++) {
        noise_fe_sq(t2, t2);
    }
    noise_fe_mul(t1, t2, t1);                            /* 2^250 - 1 */
    noise_fe_sq(t1, t1);
    for (i = 1; i < 5; i++) {
        noise_fe_sq(t1, t1);
    }
    noise_fe_mul(out, t1, t0);                           /* 2^255 - 21 */

    noise_crypto_wipe(t0, sizeof t0);
    noise_crypto_wipe(t1, sizeof t1);
    noise_crypto_wipe(t2, sizeof t2);
    noise_crypto_wipe(t3, sizeof t3);
}

static void noise_x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t e[32];
    noise_fe x1, x2, z2, x3, z3, tmp0, tmp1;
    int32_t swap = 0;
    int pos;

    memcpy(e, scalar, 32);
    e[0] &= 248u;   /* clear bits 0, 1, 2 */
    e[31] &= 127u;  /* clear bit 7 */
    e[31] |= 64u;   /* set bit 6 */

    noise_fe_frombytes(x1, point);
    noise_fe_1(x2);
    noise_fe_0(z2);
    noise_fe_copy(x3, x1);
    noise_fe_1(z3);

    for (pos = 254; pos >= 0; pos--) {
        int32_t b = (int32_t)((e[pos >> 3] >> (pos & 7)) & 1);
        swap ^= b;
        noise_fe_cswap(x2, x3, swap);
        noise_fe_cswap(z2, z3, swap);
        swap = b;

        /* A = x2+z2, B = x2-z2, C = x3+z3, D = x3-z3 */
        noise_fe_sub(tmp0, x3, z3);       /* D  */
        noise_fe_sub(tmp1, x2, z2);       /* B  */
        noise_fe_add(x2, x2, z2);         /* A  */
        noise_fe_add(z2, x3, z3);         /* C  */
        noise_fe_mul(z3, tmp0, x2);       /* DA */
        noise_fe_mul(z2, z2, tmp1);       /* CB */
        noise_fe_sq(tmp0, tmp1);          /* BB */
        noise_fe_sq(tmp1, x2);            /* AA */
        noise_fe_add(x3, z3, z2);         /* DA + CB */
        noise_fe_sub(z2, z3, z2);         /* DA - CB */
        noise_fe_mul(x2, tmp1, tmp0);     /* x2 = AA * BB */
        noise_fe_sub(tmp1, tmp1, tmp0);   /* E = AA - BB */
        noise_fe_sq(z2, z2);              /* (DA - CB)^2 */
        noise_fe_mul121666(z3, tmp1);     /* a24 * E */
        noise_fe_sq(x3, x3);              /* (DA + CB)^2 */
        noise_fe_add(tmp0, tmp0, z3);     /* BB + a24*E */
        noise_fe_mul(z3, x1, z2);         /* x3 = x1 * (DA - CB)^2 */
        noise_fe_mul(z2, tmp1, tmp0);     /* z2 = E * (AA + a24*E) */
    }

    noise_fe_cswap(x2, x3, swap);
    noise_fe_cswap(z2, z3, swap);

    noise_fe_invert(z2, z2);
    noise_fe_mul(x2, x2, z2);
    noise_fe_tobytes(out, x2);

    noise_crypto_wipe(e, sizeof e);
    noise_crypto_wipe(x1, sizeof x1);
    noise_crypto_wipe(x2, sizeof x2);
    noise_crypto_wipe(z2, sizeof z2);
    noise_crypto_wipe(x3, sizeof x3);
    noise_crypto_wipe(z3, sizeof z3);
    noise_crypto_wipe(tmp0, sizeof tmp0);
    noise_crypto_wipe(tmp1, sizeof tmp1);
}

void noise_x25519_private_key(uint8_t out[32], const uint8_t entropy[32])
{
    memcpy(out, entropy, 32);
    out[0] &= 248u;
    out[31] &= 127u;
    out[31] |= 64u;
}

void noise_x25519_public_key(uint8_t out[32], const uint8_t private_key[32])
{
    static const uint8_t basepoint[32] = { 9u };
    noise_x25519_scalarmult(out, private_key, basepoint);
}

void noise_x25519(uint8_t out[32], const uint8_t private_key[32], const uint8_t peer_public_key[32])
{
    noise_x25519_scalarmult(out, private_key, peer_public_key);
}
