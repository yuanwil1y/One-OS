#!/usr/bin/env python3
"""Transport-frame vectors for the One-OS Noise session.

Extends tools/reference/noise_nnpsk0_reference.py past the handshake: with the
transport key that handshake produces, encrypt a short sequence of ESPHome-style
packets and print them. tests/esphome_l2/test_noise.c pins them, so the frame
counter, the nonce construction and the AEAD are checked against an independent
implementation over several messages rather than one.
"""

from __future__ import annotations

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

# From the handshake in noise_nnpsk0_reference.py.
SEND_KEY = bytes.fromhex("cd9067a80a2b24de88fe47f3c6ff301b1ba2a4bdd1529099d2d4209dcb833e71")
RECV_KEY = bytes.fromhex("4da38b2c270c0777ffd85cceeb36cace757d54b028e24d582fec081e84a209c9")


def nonce_bytes(n: int) -> bytes:
    return bytes(4) + n.to_bytes(8, "little")


def frame(key: bytes, n: int, msg_type: int, payload: bytes) -> bytes:
    header = bytes(
        ((msg_type >> 8) & 0xFF, msg_type & 0xFF, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF)
    )
    ct = ChaCha20Poly1305(key).encrypt(nonce_bytes(n), header + payload, b"")
    return bytes((0x01, (len(ct) >> 8) & 0xFF, len(ct) & 0xFF)) + ct


def main() -> None:
    # The packets the client sends after the handshake, in order.
    client = [
        (1, b""),                                   # HelloRequest
        (9, b""),                                   # DeviceInfoRequest
        (11, b""),                                  # ListEntitiesRequest
        (20, b""),                                  # SubscribeStatesRequest
        (33, bytes.fromhex("11111111") + b"\x08\x00"),  # SwitchCommand, key + state=false
    ]
    print("== initiator -> responder (send key) ==")
    for n, (t, payload) in enumerate(client):
        print(f"n={n} type={t} {frame(SEND_KEY, n, t, payload).hex()}")

    # And one the responder sends back.
    print("== responder -> initiator (recv key) ==")
    server = [(2, b"\x0a\x00"), (26, bytes.fromhex("11111111") + b"\x08\x01")]
    for n, (t, payload) in enumerate(server):
        print(f"n={n} type={t} {frame(RECV_KEY, n, t, payload).hex()}")

    print("== C fixtures ==")
    sent = [frame(SEND_KEY, n, t, p) for n, (t, p) in enumerate(client)]
    recvd = [frame(RECV_KEY, n, t, p) for n, (t, p) in enumerate(server)]
    for name, value in (("SENT", sent), ("RECVD", recvd)):
        for i, f in enumerate(value):
            print(fixture(f"{name}_{i}", f))

    print()
    print("== C test (paste into tests/esphome_l2/test_noise.c) ==")
    print(test_function(client, server, sent, recvd))


def hex_bytes(data: bytes) -> str:
    return ", ".join(f"0x{b:02x}" for b in data)


def fixture(name: str, data: bytes) -> str:
    """One C array, wrapped so the line stays inside the repo's 100 columns."""
    body = hex_bytes(data)
    lines = []
    current = ""
    for token in body.split(", "):
        piece = token if not current else current + ", " + token
        if len(piece) > 88:
            lines.append(current)
            current = token
        else:
            current = piece
    lines.append(current)
    indent = " " * (len(f"static const uint8_t {name}[{len(data)}] = {{ "))
    return (
        f"static const uint8_t {name}[{len(data)}] = {{ "
        + (",\n" + indent).join(lines)
        + "};"
    )


def c_payload(payload: bytes) -> str:
    if not payload:
        return "NULL"
    return "(const uint8_t *)\"" + "".join(f"\\x{b:02x}" for b in payload) + "\""


def test_function(client, server, sent, recvd) -> str:
    """Emit the multi-frame conformance test, sized from the vectors above."""
    max_sent = max(len(f) for f in sent)
    max_recvd = max(len(f) for f in recvd)
    max_payload = max(len(p) for _, p in client + server)
    rows = "\n".join(
        f"        {{{t}u, {c_payload(p)}, {len(p)}u}}," for t, p in client
    )
    recv_rows = "\n".join(
        f"        {{{f}, sizeof({f}), {t}u, {c_payload(p)}, {len(p)}u}},"
        for (t, p), f in zip(server, (f"RECVD_{i}" for i in range(len(server))))
    )
    sent_rows = "\n".join(
        f"        {{{f}, sizeof({f})}}," for f in (f"SENT_{i}" for i in range(len(sent)))
    )
    return f"""/* Frames pinned by tools/reference/noise_transport_reference.py: the same
 * transport keys as the reference handshake, driven through a short packet
 * sequence. This is the only place the frame counter is checked against an
 * independent implementation over more than one message, so a nonce that
 * stops advancing, advances twice, or is built in the wrong byte order fails
 * here rather than silently breaking interop on the second packet. */
static void test_transport_frame_sequence(void)
{{
    static const struct {{
        uint8_t type;
        const uint8_t *payload;
        size_t payload_len;
    }} sent_plain[] = {{
{rows}
    }};
    static const struct {{
        const uint8_t *frame;
        size_t frame_len;
    }} sent_frames[] = {{
{sent_rows}
    }};
    static const struct {{
        const uint8_t *frame;
        size_t frame_len;
        uint8_t type;
        const uint8_t *payload;
        size_t payload_len;
    }} recvd_frames[] = {{
{recv_rows}
    }};

    esphome_noise_cipherstate_t send, recv;
    uint8_t frame[{max_sent}];
    uint8_t plain[{max_payload}];
    size_t frame_len = 0;
    size_t plain_len = 0;
    size_t i;

    CHECK(sizeof(sent_plain) / sizeof(sent_plain[0]) == sizeof(sent_frames) / sizeof(sent_frames[0]),
          "fixture tables disagree on the number of client packets");

    CHECK(esphome_noise_cipherstate_init(&send, SEND_KEY) == ESPHOME_NOISE_OK, "send cipher init failed");
    CHECK(esphome_noise_cipherstate_init(&recv, RECV_KEY) == ESPHOME_NOISE_OK, "recv cipher init failed");

    for (i = 0u; i < sizeof(sent_plain) / sizeof(sent_plain[0]); i++) {{
        uint8_t msg_type = sent_plain[i].type;
        uint8_t header[2] = {{0x00, msg_type}};

        CHECK(send.nonce == (uint64_t)i, "send nonce is %llu before packet %u",
              (unsigned long long)send.nonce, (unsigned)i);

        /* The session encrypts each ESPHome frame as [0x00][type][len:2 BE] plus
         * the body and the 16-byte tag, so the header is fed as associated data
         * rather than as part of the plaintext. */
        CHECK(esphome_noise_encrypt(&send, header, sizeof(header), sent_plain[i].payload,
                                    sent_plain[i].payload_len, frame, sizeof(frame), &frame_len) ==
                  ESPHOME_NOISE_OK,
              "packet %u failed to encrypt", (unsigned)i);
        CHECK(frame_len == sent_frames[i].frame_len, "packet %u is %u bytes, expected %u",
              (unsigned)i, (unsigned)frame_len, (unsigned)sent_frames[i].frame_len);
        CHECK(same(frame, sent_frames[i].frame, sent_frames[i].frame_len),
              "packet %u does not match the reference bytes", (unsigned)i);
        CHECK(send.nonce == (uint64_t)i + 1u, "send nonce did not advance exactly once on packet %u",
              (unsigned)i);
    }}

    for (i = 0u; i < sizeof(recvd_frames) / sizeof(recvd_frames[0]); i++) {{
        uint8_t header[2] = {{0x00, recvd_frames[i].type}};

        CHECK(recv.nonce == (uint64_t)i, "recv nonce is %llu before frame %u",
              (unsigned long long)recv.nonce, (unsigned)i);
        CHECK(esphome_noise_decrypt(&recv, header, sizeof(header), recvd_frames[i].frame,
                                    recvd_frames[i].frame_len, plain, sizeof(plain), &plain_len) ==
                  ESPHOME_NOISE_OK,
              "frame %u failed to decrypt", (unsigned)i);
        CHECK(plain_len == recvd_frames[i].payload_len, "frame %u decrypted to %u bytes, expected %u",
              (unsigned)i, (unsigned)plain_len, (unsigned)recvd_frames[i].payload_len);
        CHECK(same(plain, recvd_frames[i].payload, recvd_frames[i].payload_len),
              "frame %u decrypted to the wrong payload", (unsigned)i);
        CHECK(recv.nonce == (uint64_t)i + 1u, "recv nonce did not advance exactly once on frame %u",
              (unsigned)i);
    }}

    /* The first client packet is the ESPHome client hello: the handshake
     * marker (0x00 0x01 0x00 0x00) is sent before the framing starts, and this
     * is the first framed packet after it. Its length is pinned so that a
     * reader of these vectors can tell where the marker ends and the ciphertext
     * begins without counting bytes by hand. */
    CHECK(sent_frames[0].frame_len == 21u, "the first client frame is %u bytes, expected 21",
          (unsigned)sent_frames[0].frame_len);
    CHECK(sent_plain[0].type == 1u, "the first client packet is not HelloRequest");
    CHECK(sent_plain[0].payload_len == 0u, "HelloRequest must have an empty body");

    esphome_noise_cipherstate_wipe(&send);
    esphome_noise_cipherstate_wipe(&recv);
}}"""


if __name__ == "__main__":
    main()
