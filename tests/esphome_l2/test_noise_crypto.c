/*
 * test_noise_crypto.c — host test vectors for esphome_noise_crypto.
 *
 * Every expected value below is either taken verbatim from an RFC
 * (FIPS 180-4 / RFC 6234, RFC 4231, RFC 5869, RFC 8439, RFC 7748) or was
 * produced by an independent implementation (Node.js/OpenSSL 3 and a BigInt
 * X25519 ladder) as a cross-check for the cases the RFCs do not cover.
 *
 * The file also contains a second, deliberately naive SHA-256/HMAC/HKDF
 * implementation (rolling 16-word message schedule, byte-at-a-time input) that
 * is used to cross-check the HKDF framing.
 *
 * Plain C program: exits non-zero on the first failure with a message naming
 * the failing vector, prints one line per case and a final
 * "noise crypto tests: ok".
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esphome_noise_crypto.h"

/* ---------------------------------------------------------------------- */
/* tiny harness                                                            */
/* ---------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static size_t hex2bin(uint8_t *out, const char *hex)
{
    size_t n = 0;
    while (hex[0] != '\0' && hex[1] != '\0') {
        int hi = hexval(hex[0]);
        int lo = hexval(hex[1]);
        if (hi < 0 || lo < 0) {
            printf("internal test error: bad hex constant\n");
            exit(2);
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static const char *hexstr(const uint8_t *p, size_t n)
{
    static char bufs[2][2048];
    static int which = 0;
    char *b = bufs[which];
    size_t i;
    which ^= 1;
    for (i = 0; i < n && i < 1000u; i++) {
        static const char digits[] = "0123456789abcdef";
        b[2u * i] = digits[(p[i] >> 4) & 0x0fu];
        b[2u * i + 1u] = digits[p[i] & 0x0fu];
    }
    b[2u * (n < 1000u ? n : 1000u)] = '\0';
    return b;
}

static int same_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void fail_here(const char *name, const char *what)
{
    printf("  FAIL %s: %s\n", name, what);
    printf("noise crypto tests: failed\n");
    exit(1);
}

static void ok(const char *name)
{
    printf("  ok   %s\n", name);
}

static void begin(const char *section)
{
    printf("%s\n", section);
}

/* compare against a hex constant */
static void check_hex(const char *name, const uint8_t *got, size_t got_len, const char *want_hex)
{
    uint8_t want[1024];
    size_t want_len = hex2bin(want, want_hex);
    if (got_len != want_len) {
        printf("  FAIL %s: length %u, expected %u\n", name, (unsigned)got_len, (unsigned)want_len);
        printf("    got  %s\n", hexstr(got, got_len));
        printf("noise crypto tests: failed\n");
        exit(1);
    }
    if (!same_bytes(got, want, want_len)) {
        printf("  FAIL %s\n    got  %s\n    want %s\n", name, hexstr(got, got_len), want_hex);
        printf("noise crypto tests: failed\n");
        exit(1);
    }
    ok(name);
}

static void check_true(const char *name, int cond)
{
    if (!cond) {
        fail_here(name, "condition is false");
    }
    ok(name);
}

static void check_zero(const char *name, const uint8_t *got, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (got[i] != 0u) {
            printf("  FAIL %s: byte %u is 0x%02x, expected 0x00 (got %s)\n",
                   name, (unsigned)i, got[i], hexstr(got, n));
            printf("noise crypto tests: failed\n");
            exit(1);
        }
    }
    ok(name);
}

/* same check without the ok line: used inside tight loops that report once */
static void require_zero(const char *name, const uint8_t *got, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (got[i] != 0u) {
            printf("  FAIL %s: byte %u is 0x%02x, expected 0x00\n", name, (unsigned)i, got[i]);
            printf("noise crypto tests: failed\n");
            exit(1);
        }
    }
}

/* every byte must still be `v` (i.e. the call wrote nothing) */
static void require_untouched(const char *name, const uint8_t *got, size_t n, uint8_t v)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (got[i] != v) {
            printf("  FAIL %s: byte %u is 0x%02x, expected 0x%02x (nothing may be written)\n",
                   name, (unsigned)i, got[i], v);
            printf("noise crypto tests: failed\n");
            exit(1);
        }
    }
}

/* deterministic xorshift32 PRNG (mirrored in the reference generator) */
static uint32_t g_prng = 0x12345678u;

static void prng_seed(uint32_t s)
{
    g_prng = s;
}

static uint8_t prng_byte(void)
{
    g_prng ^= g_prng << 13;
    g_prng ^= g_prng >> 17;
    g_prng ^= g_prng << 5;
    return (uint8_t)(g_prng >> 24);
}

static void prng_fill(uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        b[i] = prng_byte();
    }
}

/* ---------------------------------------------------------------------- */
/* independent (naive) SHA-256 / HMAC / HKDF used as a cross-check         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t st[8];
    uint8_t blk[64];
    size_t n;
    uint64_t bits;
} naive_sha256_ctx;

static uint32_t naive_rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void naive_compress(uint32_t st[8], const uint8_t blk[64])
{
    static const uint32_t k[64] = {
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
    uint32_t w[16];
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    unsigned i;

    for (i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)blk[4u * i] << 24) | ((uint32_t)blk[4u * i + 1u] << 16) |
               ((uint32_t)blk[4u * i + 2u] << 8) | (uint32_t)blk[4u * i + 3u];
    }

    for (i = 0u; i < 64u; i++) {
        uint32_t wi;
        uint32_t s1, ch, t1, s0, maj, t2;
        if (i >= 16u) {
            /* rolling 16-word schedule: w[i&15] holds W[i-16] right now */
            uint32_t x = w[(i + 1u) & 15u];   /* W[i-15] */
            uint32_t y = w[(i + 14u) & 15u];  /* W[i-2]  */
            uint32_t s0i = naive_rotr(x, 7u) ^ naive_rotr(x, 18u) ^ (x >> 3);
            uint32_t s1i = naive_rotr(y, 17u) ^ naive_rotr(y, 19u) ^ (y >> 10);
            w[i & 15u] = w[i & 15u] + s0i + w[(i + 9u) & 15u] + s1i;
        }
        wi = w[i & 15u];
        s1 = naive_rotr(e, 6u) ^ naive_rotr(e, 11u) ^ naive_rotr(e, 25u);
        ch = (e & f) ^ (~e & g);
        t1 = h + s1 + ch + k[i] + wi;
        s0 = naive_rotr(a, 2u) ^ naive_rotr(a, 13u) ^ naive_rotr(a, 22u);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void naive_sha256_init(naive_sha256_ctx *c)
{
    c->st[0] = 0x6a09e667u; c->st[1] = 0xbb67ae85u;
    c->st[2] = 0x3c6ef372u; c->st[3] = 0xa54ff53au;
    c->st[4] = 0x510e527fu; c->st[5] = 0x9b05688cu;
    c->st[6] = 0x1f83d9abu; c->st[7] = 0x5be0cd19u;
    c->n = 0u;
    c->bits = 0u;
}

static void naive_sha256_update(naive_sha256_ctx *c, const uint8_t *p, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        c->blk[c->n++] = p[i];
        if (c->n == 64u) {
            naive_compress(c->st, c->blk);
            c->n = 0u;
        }
    }
    c->bits += (uint64_t)len * 8u;
}

static void naive_sha256_final(naive_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bits;
    unsigned i;
    c->blk[c->n++] = 0x80u;
    if (c->n > 56u) {
        while (c->n < 64u) {
            c->blk[c->n++] = 0u;
        }
        naive_compress(c->st, c->blk);
        c->n = 0u;
    }
    while (c->n < 56u) {
        c->blk[c->n++] = 0u;
    }
    for (i = 0u; i < 8u; i++) {
        c->blk[56u + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    naive_compress(c->st, c->blk);
    for (i = 0u; i < 8u; i++) {
        out[4u * i] = (uint8_t)(c->st[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(c->st[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(c->st[i] >> 8);
        out[4u * i + 3u] = (uint8_t)c->st[i];
    }
}

static void naive_sha256(const uint8_t *p, size_t len, uint8_t out[32])
{
    naive_sha256_ctx c;
    naive_sha256_init(&c);
    naive_sha256_update(&c, p, len);
    naive_sha256_final(&c, out);
}

static void naive_hmac(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                       uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    naive_sha256_ctx c;
    size_t i;

    memset(k, 0, sizeof k);
    if (key_len > 64u) {
        naive_sha256(key, key_len, inner);
        memcpy(k, inner, 32u);
    } else if (key_len > 0u) {
        memcpy(k, key, key_len);
    }

    for (i = 0u; i < 64u; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    naive_sha256_init(&c);
    naive_sha256_update(&c, pad, 64u);
    naive_sha256_update(&c, msg, msg_len);
    naive_sha256_final(&c, inner);

    for (i = 0u; i < 64u; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    naive_sha256_init(&c);
    naive_sha256_update(&c, pad, 64u);
    naive_sha256_update(&c, inner, 32u);
    naive_sha256_final(&c, out);
}

static void naive_hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
                       uint8_t *o1, uint8_t *o2, uint8_t *o3, unsigned n)
{
    uint8_t tk[32], buf[33];

    naive_hmac(ck, 32u, ikm, ikm_len, tk);
    buf[0] = 0x01u;
    naive_hmac(tk, 32u, buf, 1u, o1);
    if (n >= 2u) {
        memcpy(buf, o1, 32u);
        buf[32] = 0x02u;
        naive_hmac(tk, 32u, buf, 33u, o2);
    }
    if (n >= 3u) {
        memcpy(buf, o2, 32u);
        buf[32] = 0x03u;
        naive_hmac(tk, 32u, buf, 33u, o3);
    }
}

/* ---------------------------------------------------------------------- */
/* 1. SHA-256                                                             */
/* ---------------------------------------------------------------------- */

static void test_sha256(void)
{
    uint8_t out[NOISE_CRYPTO_SHA256_BYTES];
    uint8_t buf[1000];
    uint8_t oneshot[NOISE_CRYPTO_SHA256_BYTES];
    size_t i;

    begin("SHA-256 (FIPS 180-4 / RFC 6234)");

    noise_sha256("", 0u, out);
    check_hex("sha256(\"\")", out, 32u,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    noise_sha256("abc", 3u, out);
    check_hex("sha256(\"abc\")", out, 32u,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* 448-bit vector */
    {
        static const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        noise_sha256(m, strlen(m), out);
        check_hex("sha256(448-bit vector)", out, 32u,
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }

    /* 896-bit vector */
    {
        static const char *m =
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
            "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        noise_sha256(m, strlen(m), out);
        check_hex("sha256(896-bit vector)", out, 32u,
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    }

    /* 1,000,000 x 'a' */
    {
        noise_sha256_t ctx;
        memset(buf, 'a', 1000u);
        noise_sha256_init(&ctx);
        for (i = 0u; i < 1000u; i++) {
            noise_sha256_update(&ctx, buf, 1000u);
        }
        noise_sha256_final(&ctx, out);
        check_hex("sha256(1,000,000 x 'a')", out, 32u,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* streaming: 1000 pseudo-random bytes one-shot vs 1/3/7/63-byte chunks */
    {
        static const size_t chunks[4] = { 1u, 3u, 7u, 63u };
        unsigned c;
        prng_seed(0x12345678u);
        prng_fill(buf, sizeof buf);
        noise_sha256(buf, sizeof buf, oneshot);
        check_hex("sha256(1000 pseudo-random bytes, one-shot)", oneshot, 32u,
                  "e09228c48f5e3f11636ad620783ef5f8f4c0ba9edd1fc75f5fda0f6d4076386a");
        for (c = 0u; c < 4u; c++) {
            noise_sha256_t ctx;
            size_t off = 0u;
            char label[64];
            noise_sha256_init(&ctx);
            noise_sha256_update(&ctx, NULL, 0u); /* zero-length update must be a no-op */
            while (off < sizeof buf) {
                size_t take = chunks[c];
                if (take > sizeof buf - off) {
                    take = sizeof buf - off;
                }
                noise_sha256_update(&ctx, buf + off, take);
                off += take;
            }
            noise_sha256_final(&ctx, out);
            sprintf(label, "sha256(1000 bytes streamed in %u-byte chunks)", (unsigned)chunks[c]);
            check_hex(label, out, 32u, hexstr(oneshot, 32u));
        }
    }
}

/* ---------------------------------------------------------------------- */
/* 2. HMAC-SHA256 (RFC 4231)                                              */
/* ---------------------------------------------------------------------- */

static void test_hmac(void)
{
    uint8_t key[200];
    uint8_t data[200];
    uint8_t out[NOISE_CRYPTO_SHA256_BYTES];
    uint8_t ref[NOISE_CRYPTO_SHA256_BYTES];

    begin("HMAC-SHA256 (RFC 4231)");

    /* case 1 */
    memset(key, 0x0b, 20u);
    noise_hmac_sha256(key, 20u, "Hi There", 8u, out);
    check_hex("hmac_sha256 RFC4231 case 1", out, 32u,
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    /* case 2: key shorter than the block size */
    noise_hmac_sha256((const uint8_t *)"Jefe", 4u, "what do ya want for nothing?", 28u, out);
    check_hex("hmac_sha256 RFC4231 case 2 (short key)", out, 32u,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* case 3 */
    memset(key, 0xaa, 20u);
    memset(data, 0xdd, 50u);
    noise_hmac_sha256(key, 20u, data, 50u, out);
    check_hex("hmac_sha256 RFC4231 case 3", out, 32u,
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    /* case 4: binary key */
    {
        unsigned i;
        for (i = 0u; i < 25u; i++) {
            key[i] = (uint8_t)(i + 1u);
        }
    }
    memset(data, 0xcd, 50u);
    noise_hmac_sha256(key, 25u, data, 50u, out);
    check_hex("hmac_sha256 RFC4231 case 4 (binary key)", out, 32u,
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

    /* case 6: 131-byte key, key must be hashed first */
    memset(key, 0xaa, 131u);
    noise_hmac_sha256(key, 131u, "Test Using Larger Than Block-Size Key - Hash Key First", 54u, out);
    check_hex("hmac_sha256 RFC4231 case 6 (131-byte key)", out, 32u,
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    /* case 7: 131-byte key and long data */
    noise_hmac_sha256(key, 131u,
                      "This is a test using a larger than block-size key and a larger "
                      "than block-size data. The key needs to be hashed before being "
                      "used by the HMAC algorithm.",
                      152u, out);
    check_hex("hmac_sha256 RFC4231 case 7 (131-byte key, long data)", out, 32u,
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

    /* case 7 via the streaming interface, fed byte by byte */
    {
        static const char *m7 =
            "This is a test using a larger than block-size key and a larger "
            "than block-size data. The key needs to be hashed before being "
            "used by the HMAC algorithm.";
        noise_hmac_sha256_t ctx;
        size_t i;
        noise_hmac_sha256_init(&ctx, key, 131u);
        for (i = 0u; i < strlen(m7); i++) {
            noise_hmac_sha256_update(&ctx, m7 + i, 1u);
        }
        noise_hmac_sha256_final(&ctx, ref);
        check_hex("hmac_sha256 case 7 streamed", ref, 32u, hexstr(out, 32u));
    }

    /* 200-byte key: must equal the HMAC computed with the pre-hashed key */
    memset(key, 0x5a, 200u);
    noise_hmac_sha256(key, 200u, "noise crypto 200-byte key check", 31u, out);
    check_hex("hmac_sha256 200-byte key", out, 32u,
              "7f475a3ccfa554e21033b04d13bf93a6475623ffb3ebec4ac24160e2247f2039");
    {
        uint8_t hashed_key[NOISE_CRYPTO_SHA256_BYTES];
        noise_sha256(key, 200u, hashed_key);
        noise_hmac_sha256(hashed_key, 32u, "noise crypto 200-byte key check", 31u, ref);
        check_hex("hmac_sha256 200-byte key == hmac with key hashed first", ref, 32u,
                  hexstr(out, 32u));
    }

    /* empty key and empty message are valid */
    {
        static const uint8_t empty_key_hmac[32] = {
            0xb6u, 0x13u, 0x67u, 0x9au, 0x08u, 0x14u, 0xd9u, 0xecu,
            0x77u, 0x2fu, 0x95u, 0xd7u, 0x78u, 0xc3u, 0x5fu, 0xc5u,
            0xffu, 0x16u, 0x97u, 0xc4u, 0x93u, 0x71u, 0x56u, 0x53u,
            0xc6u, 0xc7u, 0x12u, 0x14u, 0x42u, 0x92u, 0xc5u, 0xadu
        };
        noise_hmac_sha256(NULL, 0u, NULL, 0u, out);
        check_hex("hmac_sha256(empty key, empty message)", out, 32u, hexstr(empty_key_hmac, 32u));
    }
}

/* ---------------------------------------------------------------------- */
/* 3. HKDF                                                                */
/* ---------------------------------------------------------------------- */

static void test_hkdf(void)
{
    uint8_t out1[32], out2[32], out3[32];
    uint8_t n1[32], n2[32], n3[32];

    begin("HKDF-SHA256 (RFC 5869) and Noise framing");

    /* --- RFC 5869 test case 1, verified by manual HMAC chaining --- */
    {
        uint8_t ikm[22], salt[13], info[10];
        uint8_t prk[32], t1[32], t2[32], okm[42];
        uint8_t buf[64];

        memset(ikm, 0x0b, sizeof ikm);
        hex2bin(salt, "000102030405060708090a0b0c");
        hex2bin(info, "f0f1f2f3f4f5f6f7f8f9");

        /* PRK = HMAC-Hash(salt, IKM) */
        noise_hmac_sha256(salt, sizeof salt, ikm, sizeof ikm, prk);
        check_hex("hkdf RFC5869 case 1 PRK = HMAC(salt, IKM)", prk, 32u,
                  "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");

        /* T(1) = HMAC(PRK, info || 0x01) */
        memcpy(buf, info, sizeof info);
        buf[sizeof info] = 0x01u;
        noise_hmac_sha256(prk, 32u, buf, sizeof info + 1u, t1);

        /* T(2) = HMAC(PRK, T(1) || info || 0x02) */
        memcpy(buf, t1, 32u);
        memcpy(buf + 32u, info, sizeof info);
        buf[32u + sizeof info] = 0x02u;
        noise_hmac_sha256(prk, 32u, buf, 32u + sizeof info + 1u, t2);

        /* OKM = first 42 bytes of T(1) || T(2) || ... */
        memcpy(okm, t1, 32u);
        memcpy(okm + 32u, t2, 10u);
        check_hex("hkdf RFC5869 case 1 OKM (42 bytes, manual HMAC chaining)", okm, 42u,
                  "3cb25f25faacd57a90434f64d0362f2a"
                  "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                  "34007208d5b887185865");
    }

    /* --- Noise HKDF() framing: chaining_key = ikm = 32 zero bytes --- */
    {
        uint8_t ck[32], ikm[32];
        memset(ck, 0, sizeof ck);
        memset(ikm, 0, sizeof ikm);
        noise_hkdf_sha256(ck, ikm, sizeof ikm, out1, out2, out3, 3u);
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out1", out1, 32u,
                  "df7204546f1bee78b85324a7898ca119b387e01386d1aef037781d4a8a036aee");
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out2", out2, 32u,
                  "a7b65a6e7f873068dd147c56493e71294acc89e73baae2e4a87075f18739b4cd");
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out3", out3, 32u,
                  "f0743cc51d27f9b81c0481d34c1e9d42410bda49d6d389387589a364b790742e");
        naive_hkdf(ck, ikm, sizeof ikm, n1, n2, n3, 3u);
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out1 vs naive SHA-256", out1, 32u, hexstr(n1, 32u));
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out2 vs naive SHA-256", out2, 32u, hexstr(n2, 32u));
        check_hex("noise hkdf(ck=0^32, ikm=0^32) out3 vs naive SHA-256", out3, 32u, hexstr(n3, 32u));
    }

    /* --- ikm_len == 0 with ikm == NULL --- */
    {
        uint8_t ck[32];
        memset(ck, 0, sizeof ck);
        noise_hkdf_sha256(ck, NULL, 0u, out1, out2, out3, 3u);
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out1", out1, 32u,
                  "eb70f01dede9afafa449eee1b1286504e1f62388b3f7dd4f956697b0e828fe18");
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out2", out2, 32u,
                  "1e59c2ec0fe6e7e7ac2613b6ab65342a83379969da234240cded3777914db907");
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out3", out3, 32u,
                  "5568c74fdb8fc92331d5c59e1e2dd77a8c2c63aba7cf2d3457f8ee8620462f8a");
        naive_hkdf(ck, NULL, 0u, n1, n2, n3, 3u);
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out1 vs naive SHA-256", out1, 32u, hexstr(n1, 32u));
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out2 vs naive SHA-256", out2, 32u, hexstr(n2, 32u));
        check_hex("noise hkdf(ck=0^32, ikm_len=0) out3 vs naive SHA-256", out3, 32u, hexstr(n3, 32u));
    }

    /* --- pseudo-random chaining key and ikm, cross-checked against naive --- */
    {
        uint8_t ck[32], ikm[32];
        prng_seed(0xfeedfaceu);
        prng_fill(ck, sizeof ck);
        prng_fill(ikm, sizeof ikm);
        noise_hkdf_sha256(ck, ikm, sizeof ikm, out1, out2, out3, 3u);
        naive_hkdf(ck, ikm, sizeof ikm, n1, n2, n3, 3u);
        check_hex("noise hkdf(prng ck, prng ikm) out1 vs naive SHA-256", out1, 32u, hexstr(n1, 32u));
        check_hex("noise hkdf(prng ck, prng ikm) out2 vs naive SHA-256", out2, 32u, hexstr(n2, 32u));
        check_hex("noise hkdf(prng ck, prng ikm) out3 vs naive SHA-256", out3, 32u, hexstr(n3, 32u));
    }

    /* --- num_outputs == 2 must not touch out3 --- */
    {
        uint8_t ck[32], ikm[32];
        memset(ck, 0, sizeof ck);
        memset(ikm, 0, sizeof ikm);
        memset(out3, 0x5a, sizeof out3);
        noise_hkdf_sha256(ck, ikm, sizeof ikm, out1, out2, out3, 2u);
        check_true("noise hkdf num_outputs=2 leaves out3 untouched", out3[0] == 0x5au && out3[31] == 0x5au);
    }
}

/* ---------------------------------------------------------------------- */
/* 4. ChaCha20-Poly1305 AEAD (RFC 8439)                                   */
/* ---------------------------------------------------------------------- */

/* encrypt+compare, then decrypt+compare */
static void aead_case(const char *name, const char *key_hex, const char *nonce_hex,
                      const char *ad_hex, const uint8_t *pt, size_t pt_len,
                      const char *ct_hex, const char *tag_hex)
{
    uint8_t key[32], nonce[12];
    uint8_t ad[64];
    uint8_t want_ct[512];
    uint8_t want_tag[16];
    uint8_t out[640];
    uint8_t back[640];
    size_t ad_len, ct_len;

    hex2bin(key, key_hex);
    hex2bin(nonce, nonce_hex);
    ad_len = hex2bin(ad, ad_hex);
    ct_len = hex2bin(want_ct, ct_hex);
    hex2bin(want_tag, tag_hex);

    if (strlen(key_hex) != 64u || strlen(nonce_hex) != 24u) {
        fail_here(name, "internal test error: key must be 32 bytes and nonce 12 bytes");
    }
    if (ct_len != pt_len) {
        fail_here(name, "internal test error: ciphertext/plaintext length mismatch");
    }
    if (ad_len > sizeof ad) {
        fail_here(name, "internal test error: aad too long");
    }

    memset(out, 0x5a, sizeof out);
    if (!noise_aead_encrypt(key, nonce, ad, ad_len, pt, pt_len, out)) {
        fail_here(name, "encrypt returned false");
    }
    if (!same_bytes(out, want_ct, ct_len)) {
        printf("  FAIL %s (ciphertext)\n    got  %s\n    want %s\n",
               name, hexstr(out, ct_len), ct_hex);
        printf("noise crypto tests: failed\n");
        exit(1);
    }
    if (!same_bytes(out + ct_len, want_tag, 16u)) {
        printf("  FAIL %s (tag)\n    got  %s\n    want %s\n",
               name, hexstr(out + ct_len, 16u), tag_hex);
        printf("noise crypto tests: failed\n");
        exit(1);
    }
    if (out[ct_len + 16u] != 0x5au) {
        fail_here(name, "encrypt wrote past plaintext_len + 16 bytes");
    }

    memset(back, 0xa5, sizeof back);
    if (!noise_aead_decrypt(key, nonce, ad, ad_len, out, ct_len + 16u, back)) {
        fail_here(name, "decrypt returned false on a valid message");
    }
    if (pt_len > 0u && !same_bytes(back, pt, pt_len)) {
        printf("  FAIL %s (round trip plaintext)\n    got  %s\n    want %s\n",
               name, hexstr(back, pt_len), hexstr(pt, pt_len));
        printf("noise crypto tests: failed\n");
        exit(1);
    }
    if (back[pt_len] != 0xa5u) {
        fail_here(name, "decrypt wrote past ciphertext_len - 16 bytes");
    }

    ok(name);
}

static void test_aead(void)
{
    static const char *rfc_key = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
    static const char *rfc_nonce = "070000004041424344454647";
    static const char *rfc_aad = "50515253c0c1c2c3c4c5c6c7";
    static const char *rfc_pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    static const char *rfc_ct =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3"
        "692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7b"
        "c3ff4def08e4b7a9de576d26586cec64b6116";
    static const char *rfc_tag = "1ae10b594f09e26a7e902ecbd0600691";
    uint8_t pt[300];
    unsigned i;

    begin("ChaCha20-Poly1305 AEAD (RFC 8439 section 2.8)");

    aead_case("aead RFC8439 2.8.2 (114-byte pt, 12-byte aad)",
              rfc_key, rfc_nonce, rfc_aad, (const uint8_t *)rfc_pt, 114u, rfc_ct, rfc_tag);

    /* empty plaintext and empty AAD */
    aead_case("aead empty plaintext + empty aad (zero key/nonce)", "0000000000000000000000000000000000000000000000000000000000000000",
              "000000000000000000000000", "", (const uint8_t *)"", 0u, "",
              "4eb972c9a8fb3a1b382bb4d36f5ffad1");

    aead_case("aead empty plaintext + RFC aad", rfc_key, rfc_nonce, rfc_aad,
              (const uint8_t *)"", 0u, "", "e622e5647a38d967a7ecbcb46c7f675c");

    aead_case("aead RFC plaintext + empty aad", rfc_key, rfc_nonce, "",
              (const uint8_t *)rfc_pt, 114u, rfc_ct, "6a23a4681fd59456aea1d29f82477216");

    /* short message, with and without AAD */
    aead_case("aead 11-byte pt + 2-byte aad", rfc_key, rfc_nonce, "6164",
              (const uint8_t *)"hello noise", 11u, "f71e85316edd2ed57c91ea",
              "5f12f96be7d9c80014b8fcb5f78de84c");
    aead_case("aead 11-byte pt + no aad", rfc_key, rfc_nonce, "",
              (const uint8_t *)"hello noise", 11u, "f71e85316edd2ed57c91ea",
              "12e016fe38ad2c687246278c49a6319d");

    /* block boundaries: 32-byte key = 00..1f, nonce = f0..fb */
    {
        uint8_t ad[32];
        static const char *key_hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
        static const char *nonce_hex = "f0f1f2f3f4f5f6f7f8f9fafb";
        static const char *ad16 = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
        static const char *ad15 = "a0a1a2a3a4a5a6a7a8a9aaabacadae";
        static const char *ad20 = "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3";

        for (i = 0u; i < 64u; i++) {
            pt[i] = (uint8_t)i;
        }
        aead_case("aead 64-byte pt (exactly one block) + 16-byte aad (exactly one block)",
                  key_hex, nonce_hex, ad16, pt, 64u,
                  "c04d3cc583add8d057f791f68b2036de259ab6811696f55d259c6a1f818102ae"
                  "f73393bda4b975a073c2516d682d7064b5d390b0813db61df320335a8ce9c881",
                  "bfb79459f1bb5dce6ea5b76f8f402a0f");

        pt[64] = 0x40u;
        aead_case("aead 65-byte pt (one byte into the next block) + 15-byte aad",
                  key_hex, nonce_hex, ad15, pt, 65u,
                  "c04d3cc583add8d057f791f68b2036de259ab6811696f55d259c6a1f818102ae"
                  "f73393bda4b975a073c2516d682d7064b5d390b0813db61df320335a8ce9c881"
                  "f6",
                  "d07f63d5bba0b8e98ebdc5b03fb6d680");

        for (i = 0u; i < 300u; i++) {
            pt[i] = (uint8_t)i;
        }
        aead_case("aead 300-byte pt (5 chacha blocks) + 20-byte aad", key_hex, nonce_hex, ad20,
                  pt, 300u,
                  "c04d3cc583add8d057f791f68b2036de259ab6811696f55d259c6a1f818102ae"
                  "f73393bda4b975a073c2516d682d7064b5d390b0813db61df320335a8ce9c881"
                  "f61e61d4cb56b353f5e0c870d35991d0870e0db4cbc3b9aebcf238e63641cf52"
                  "e983c94ace759fe5c836d278ec850151e9af8ad173f33774f335354f49b71861"
                  "0f8a3050bc7860390c121e5aab010b63cf2c8964134ea2a79c3ca4d33d75b690"
                  "95481cd72dd204c1928102669fdd8052c51b45f6f6caad80b1b8945397836ea4"
                  "5452c71352abd6630697a2557604c611d02ef54e20677bc7467bbc8fd37890b5"
                  "455f2774bd83dc2e17555cc05277d1c113c4525d6226e45e1d034dbefc9bbb47"
                  "4df65f98c21f150d0c9e83dd6fb4f2d1177f419c64713227ae8b64f56f67cec5"
                  "78c1c37bcb720b0bf61684f9",
                  "c54f9519a73529a56b9d0c22fb5cb813");

        /* zero key and nonce with 32-byte pt and aad */
        for (i = 0u; i < 32u; i++) {
            ad[i] = (uint8_t)i;
            pt[i] = (uint8_t)(i ^ 0x5au);
        }
        aead_case("aead 32-byte pt + 32-byte aad with all-zero key/nonce",
                  "0000000000000000000000000000000000000000000000000000000000000000",
                  "000000000000000000000000", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                  pt, 32u, "c55cbfe70b0e6427cae9c72d257a5c58814461e906ac29245085137f74a93ea8",
                  "7eb7a6a90677ac039d6d5870b2d88f6f");
    }
}

static void test_aead_tamper(void)
{
    static const char *rfc_key = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
    static const char *rfc_nonce = "070000004041424344454647";
    static const char *rfc_aad = "50515253c0c1c2c3c4c5c6c7";
    static const char *rfc_pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    uint8_t key[32], nonce[12], ad[64], pt[128], sealed[160], out[160];
    size_t ad_len, pt_len, total;
    unsigned i, bit;

    begin("ChaCha20-Poly1305 tamper resistance");

    hex2bin(key, rfc_key);
    hex2bin(nonce, rfc_nonce);
    ad_len = hex2bin(ad, rfc_aad);
    pt_len = strlen(rfc_pt);
    memcpy(pt, rfc_pt, pt_len);
    if (!noise_aead_encrypt(key, nonce, ad, ad_len, pt, pt_len, sealed)) {
        fail_here("aead tamper setup", "encrypt failed");
    }
    total = pt_len + 16u;

    /* flip every single bit of the ciphertext, of the tag and of the AAD */
    for (i = 0u; i < total; i++) {
        for (bit = 0u; bit < 8u; bit++) {
            memset(out, 0xaa, sizeof out);
            sealed[i] ^= (uint8_t)(1u << bit);
            if (noise_aead_decrypt(key, nonce, ad, ad_len, sealed, total, out)) {
                printf("  FAIL aead tamper: flipping bit %u of byte %u was accepted\n", bit, i);
                printf("noise crypto tests: failed\n");
                exit(1);
            }
            sealed[i] ^= (uint8_t)(1u << bit);
            require_zero("aead tamper leaves no plaintext", out, pt_len);
        }
    }
    ok("aead rejects every single-bit flip of ciphertext and tag (no plaintext written)");

    for (i = 0u; i < ad_len; i++) {
        for (bit = 0u; bit < 8u; bit++) {
            memset(out, 0xaa, sizeof out);
            ad[i] ^= (uint8_t)(1u << bit);
            if (noise_aead_decrypt(key, nonce, ad, ad_len, sealed, total, out)) {
                printf("  FAIL aead tamper: flipping bit %u of aad byte %u was accepted\n", bit, i);
                printf("noise crypto tests: failed\n");
                exit(1);
            }
            ad[i] ^= (uint8_t)(1u << bit);
            require_zero("aead tamper leaves no plaintext", out, pt_len);
        }
    }
    ok("aead rejects every single-bit flip of the aad (no plaintext written)");

    /* truncating the tag (ciphertext_len < 16) must fail without touching out:
       there is no plaintext length to wipe in that case, so the contract is
       "return false and write nothing at all" */
    for (i = 0u; i < 16u; i++) {
        memset(out, 0xaa, sizeof out);
        if (noise_aead_decrypt(key, nonce, ad, ad_len, sealed, (size_t)i, out)) {
            printf("  FAIL aead decrypt accepted a %u-byte ciphertext\n", i);
            printf("noise crypto tests: failed\n");
            exit(1);
        }
        require_untouched("aead decrypt ciphertext_len < 16", out, sizeof out, 0xaau);
    }
    ok("aead decrypt rejects ciphertext_len 0..15 and writes nothing");

    /* a valid message still round-trips after all that tampering */
    memset(out, 0, sizeof out);
    if (!noise_aead_decrypt(key, nonce, ad, ad_len, sealed, total, out)) {
        fail_here("aead untampered decrypt", "decrypt failed");
    }
    check_true("aead untampered message still verifies", same_bytes(out, pt, pt_len));
}

/* ---------------------------------------------------------------------- */
/* 5. X25519 (RFC 7748)                                                   */
/* ---------------------------------------------------------------------- */

static void x25519_case(const char *name, const char *scalar_hex, const char *u_hex,
                        const char *want_hex)
{
    uint8_t scalar[32], u[32], out[32];

    hex2bin(scalar, scalar_hex);
    hex2bin(u, u_hex);
    noise_x25519(out, scalar, u);
    check_hex(name, out, 32u, want_hex);
}

static void test_x25519_vectors(void)
{
    static const char *v1_scalar = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
    static const char *v1_u = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
    static const char *v2_scalar = "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d";
    static const char *v2_u = "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493";
    uint8_t sk[32], out[32], entropy[32];

    begin("X25519 (RFC 7748)");

    /* section 5.2, vector 1 and 2 (raw scalar multiplication) */
    x25519_case("x25519 RFC7748 5.2 vector 1", v1_scalar, v1_u,
                "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    x25519_case("x25519 RFC7748 5.2 vector 2", v2_scalar, v2_u,
                "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    /* public key derivation for the same scalars (clamped internally) */
    hex2bin(sk, v1_scalar);
    noise_x25519_public_key(out, sk);
    check_hex("x25519 public_key(RFC7748 5.2 vector 1 scalar)", out, 32u,
              "1c9fd88f45606d932a80c71824ae151d15d73e77de38e8e000852e614fae7019");
    hex2bin(sk, v2_scalar);
    noise_x25519_public_key(out, sk);
    check_hex("x25519 public_key(RFC7748 5.2 vector 2 scalar)", out, 32u,
              "ff63fe57bfbf43fa3f563628b149af704d3db625369c49983650347a6a71e00e");

    /* public_key(x) == scalarmult(x, base point u=9) */
    {
        uint8_t bp[32];
        uint8_t ref[32];
        memset(sk, 0xa7, 32u);
        noise_x25519_public_key(out, sk);
        memset(bp, 0, sizeof bp);
        bp[0] = 9u;
        noise_x25519(ref, sk, bp);
        check_hex("x25519 public_key == scalarmult by u=9", out, 32u, hexstr(ref, 32u));
    }

    /* section 6.1 Diffie-Hellman */
    {
        uint8_t alice_priv[32], bob_priv[32], alice_pub[32], bob_pub[32];
        uint8_t s1[32], s2[32];
        hex2bin(alice_priv, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        hex2bin(bob_priv, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

        noise_x25519_public_key(alice_pub, alice_priv);
        check_hex("x25519 RFC7748 6.1 Alice public key", alice_pub, 32u,
                  "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
        noise_x25519_public_key(bob_pub, bob_priv);
        check_hex("x25519 RFC7748 6.1 Bob public key", bob_pub, 32u,
                  "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

        noise_x25519(s1, alice_priv, bob_pub);
        check_hex("x25519 RFC7748 6.1 shared secret (Alice)", s1, 32u,
                  "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        noise_x25519(s2, bob_priv, alice_pub);
        check_hex("x25519 RFC7748 6.1 shared secret (Bob)", s2, 32u, hexstr(s1, 32u));
    }

    /* private key clamping: 32-byte entropy, then the public key */
    {
        unsigned i;
        for (i = 0u; i < 32u; i++) {
            entropy[i] = (uint8_t)(0xf0u + i);
        }
        noise_x25519_private_key(out, entropy);
        check_hex("x25519 private_key clamps entropy", out, 32u,
                  "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e4f");
        noise_x25519_public_key(sk, entropy);
        check_hex("x25519 public_key of unclamped entropy", sk, 32u,
                  "3e73ce162827a32ff92378fcf4f36464d8cf0a8113638690a775f2b699abd66e");
    }

    /* clamping is idempotent and applied internally by every entry point */
    {
        uint8_t clamped[32], unclamped[32], a[32], b[32];
        unsigned i;
        for (i = 0u; i < 32u; i++) {
            unclamped[i] = 0xffu;
        }
        noise_x25519_private_key(clamped, unclamped);
        noise_x25519_public_key(a, unclamped);
        noise_x25519_public_key(b, clamped);
        check_hex("x25519 public_key clamps internally", a, 32u, hexstr(b, 32u));
        check_hex("x25519 public_key(0xff^32)", a, 32u,
                  "847c0d2c375234f365e660955187a3735a0f7613d1609d3a6a4d8c53aeaa5a22");

        {
            uint8_t bp[32];
            memset(bp, 0, sizeof bp);
            bp[0] = 9u;
            noise_x25519(a, unclamped, bp);
            noise_x25519(b, clamped, bp);
            check_hex("x25519 clamps the private key internally", a, 32u, hexstr(b, 32u));
        }
    }
}

static void test_x25519_iterated(void)
{
    uint8_t k[32], u[32], r[32];
    int i;

    begin("X25519 iterated test (RFC 7748 section 5.2)");

    memset(k, 0, sizeof k);
    k[0] = 9u;
    memcpy(u, k, sizeof u);
    noise_x25519(r, k, u);
    memcpy(u, k, sizeof u);
    memcpy(k, r, sizeof k);
    check_hex("x25519 iterated 1 time", k, 32u,
              "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");

    if (getenv("NOISE_CRYPTO_SKIP_SLOW") != NULL) {
        printf("  skip x25519 iterated 1000 times (NOISE_CRYPTO_SKIP_SLOW is set)\n");
        return;
    }

    memset(k, 0, sizeof k);
    k[0] = 9u;
    memcpy(u, k, sizeof u);
    for (i = 0; i < 1000; i++) {
        noise_x25519(r, k, u);
        memcpy(u, k, sizeof u);
        memcpy(k, r, sizeof r);
    }
    check_hex("x25519 iterated 1000 times", k, 32u,
              "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
}

/* ---------------------------------------------------------------------- */
/* 6. robustness                                                          */
/* ---------------------------------------------------------------------- */

static void test_robustness(void)
{
    uint8_t priv[32], peer[32], out[32], scratch[64];
    unsigned i;

    begin("robustness and edge cases");

    /* low-order peer public keys: all-zero output, no crash, no division by zero */
    hex2bin(priv, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    memset(peer, 0, sizeof peer);
    noise_x25519(out, priv, peer);
    check_zero("x25519 all-zero peer public key -> all-zero secret", out, 32u);

    memset(peer, 0, sizeof peer);
    peer[0] = 1u;
    noise_x25519(out, priv, peer);
    check_zero("x25519 peer public key 1 -> all-zero secret", out, 32u);

    hex2bin(peer, "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
    noise_x25519(out, priv, peer);
    check_zero("x25519 peer public key p-1 -> all-zero secret", out, 32u);

    /* u with the (ignored) high bit set is the same as without it */
    {
        uint8_t a[32], b[32], u1[32], u2[32];
        prng_seed(0x0badf00du);
        prng_fill(u1, sizeof u1);
        memcpy(u2, u1, sizeof u2);
        u2[31] |= 0x80u;
        noise_x25519(a, priv, u1);
        noise_x25519(b, priv, u2);
        check_hex("x25519 ignores the top bit of the u-coordinate", a, 32u, hexstr(b, 32u));
    }

    /* AEAD: ciphertext_len 0..15 must fail */
    {
        uint8_t key[32], nonce[12];
        memset(key, 0, sizeof key);
        memset(nonce, 0, sizeof nonce);
        for (i = 0u; i < 16u; i++) {
            memset(scratch, 0xaa, sizeof scratch);
            if (noise_aead_decrypt(key, nonce, NULL, 0u, scratch, (size_t)i, scratch + 32)) {
                printf("  FAIL aead decrypt accepted ciphertext_len %u\n", i);
                printf("noise crypto tests: failed\n");
                exit(1);
            }
        }
        ok("aead decrypt rejects every ciphertext_len in 0..15");
    }

    /* HMAC with a 200-byte key equals the HMAC with the key hashed first
       (checked with the module's own SHA-256 and with the naive one) */
    {
        uint8_t key[200];
        uint8_t hk[32], hk_naive[32], a[32], b[32], c[32];
        static const char *msg = "the quick brown fox jumps over the lazy dog";

        prng_seed(0x5eed1234u);
        prng_fill(key, sizeof key);
        noise_sha256(key, sizeof key, hk);
        naive_sha256(key, sizeof key, hk_naive);
        check_hex("sha256 of the 200-byte key (module vs naive)", hk, 32u, hexstr(hk_naive, 32u));
        noise_hmac_sha256(key, 200u, msg, strlen(msg), a);
        noise_hmac_sha256(hk, 32u, msg, strlen(msg), b);
        naive_hmac(key, 200u, (const uint8_t *)msg, strlen(msg), c);
        check_hex("hmac_sha256(200-byte key) == hmac_sha256(sha256(key))", a, 32u, hexstr(b, 32u));
        check_hex("hmac_sha256(200-byte key) vs naive implementation", a, 32u, hexstr(c, 32u));
    }

    /* wipe really clears the buffer and keeps the compiler from removing it */
    {
        uint8_t secret[64];
        memset(secret, 0x5a, sizeof secret);
        noise_crypto_wipe(secret, sizeof secret);
        check_zero("noise_crypto_wipe clears the buffer", secret, sizeof secret);
        noise_crypto_wipe(secret, 0u); /* zero length is a no-op */
        ok("noise_crypto_wipe(ptr, 0) does not touch the buffer");
    }

    /* SHA-256 final leaves the context wiped (unusable) */
    {
        noise_sha256_t ctx;
        noise_sha256_init(&ctx);
        noise_sha256_update(&ctx, "abc", 3u);
        noise_sha256_final(&ctx, out);
        check_zero("sha256_final wipes the context", (const uint8_t *)&ctx, sizeof ctx);
    }

    /* streaming HMAC with zero-length updates interleaved */
    {
        uint8_t k[32], m[64], a[32], b[32];
        noise_hmac_sha256_t hctx;
        prng_seed(0x1234abcdu);
        prng_fill(k, sizeof k);
        prng_fill(m, sizeof m);
        noise_hmac_sha256(k, sizeof k, m, sizeof m, a);
        noise_hmac_sha256_init(&hctx, k, sizeof k);
        for (i = 0u; i < sizeof m; i++) {
            noise_hmac_sha256_update(&hctx, NULL, 0u);
            noise_hmac_sha256_update(&hctx, m + i, 1u);
        }
        noise_hmac_sha256_final(&hctx, b);
        check_hex("hmac streaming byte-by-byte with empty updates", a, 32u, hexstr(b, 32u));
    }
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    printf("esphome_noise_crypto host tests\n");
    test_sha256();
    test_hmac();
    test_hkdf();
    test_aead();
    test_aead_tamper();
    test_x25519_vectors();
    test_x25519_iterated();
    test_robustness();
    printf("noise crypto tests: ok\n");
    return 0;
}
