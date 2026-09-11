/* Minimal Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder for host tests.
 *
 * Written directly against the Noise Protocol Framework (revision 34, sections
 * 5, 6 and 9) so that the initiator under test is checked against a second
 * implementation of the state machine rather than against itself. The
 * cryptographic primitives are shared with the code under test; the 43-byte-byte
 * transcript layout, token order and key schedule are not.
 *
 * Test-support code only. It is never compiled into firmware.
 */
#ifndef NOISE_TEST_RESPONDER_H
#define NOISE_TEST_RESPONDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esphome_noise.h"

#define NTR_MSG_BYTES ESPHOME_NOISE_HANDSHAKE_MSG_BYTES
#define NTR_MAX_FRAME 1024u

typedef struct {
    uint8_t ck[32];
    uint8_t h[32];
    uint8_t k[32];
    uint8_t psk[32];
    uint8_t send_key[32];
    uint8_t recv_key[32];
    uint64_t nonce;
    uint8_t e_priv[32];
    uint8_t e_pub[32];
    uint8_t re[32];
    bool ready;
    bool handshake_failed;
    bool split_done;
} ntr_state_t;

/* Deterministic bytes for tests. */
void ntr_seeded_entropy(void *user, uint8_t *out, size_t len);

/* Re-run the handshake from scratch with a fixed ephemeral key pair. */
void ntr_reset(ntr_state_t *st, const uint8_t psk[32], const uint8_t prologue[12],
               const uint8_t e_priv[32], const uint8_t e_pub[32]);

/* Read the 48-byte initiator handshake message and produce the 48-byte
 * response. Returns false (and sets handshake_failed) if the initiator's tag
 * does not authenticate, i.e. its PSK, prologue or ephemeral key is wrong. */
bool ntr_handshake(ntr_state_t *st, const uint8_t msg1[NTR_MSG_BYTES], uint8_t msg2[NTR_MSG_BYTES]);

/* Derive the transport keys. True once the handshake has completed. */
bool ntr_split(ntr_state_t *st, uint8_t server_send[32], uint8_t server_recv[32]);

/* Transport framing exactly as the ESPHome Native API peer emits it:
 * [0x01][frame_len_hi][frame_len_lo][aead(type_hi, type_lo, len_hi, len_lo, payload)] */
size_t ntr_seal(ntr_state_t *st, uint16_t type, const uint8_t *payload, size_t len, uint8_t *out,
                size_t cap);
bool ntr_open(ntr_state_t *st, const uint8_t *frame, size_t frame_len, uint16_t *type,
              uint8_t *payload, size_t cap, size_t *payload_len);

#endif /* NOISE_TEST_RESPONDER_H */
