#!/usr/bin/env python3
"""Independent reference vectors for the One-OS Noise_NNpsk0 client.

This script does not import or compile any One-OS code. It re-implements the
Noise_NNpsk0_25519_ChaChaPoly_SHA256 handshake directly from the Noise
Protocol Framework (revision 34, sections 5, 6, 9 and 12) on top of Python's
hashlib and the `cryptography` package, and prints the exact byte strings that
firmware/components/esphome_l2/esphome_noise.c must produce for the pinned
inputs below. tests/esphome_l2/test_noise.c embeds the output as fixtures, so
the C state machine is checked against a second, independent implementation
rather than against itself.

Regenerate with:  python tools/reference/noise_nnpsk0_reference.py
"""

from __future__ import annotations

import hashlib
import hmac

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

PROTOCOL_NAME = b"Noise_NNpsk0_25519_ChaChaPoly_SHA256"
PROLOGUE = b"NoiseAPIInit"
PSK = bytes(32)
CLIENT_ENTROPY = bytes(range(1, 33))
SERVER_ENTROPY = bytes(range(33, 65))

HASHLEN = 32
DHLEN = 32
TAG = 16


def hkdf(ck: bytes, ikm: bytes, n: int) -> list[bytes]:
    temp_key = hmac.new(ck, ikm, hashlib.sha256).digest()
    out1 = hmac.new(temp_key, b"\x01", hashlib.sha256).digest()
    out2 = hmac.new(temp_key, out1 + b"\x02", hashlib.sha256).digest()
    outs = [out1, out2]
    if n == 3:
        outs.append(hmac.new(temp_key, out2 + b"\x03", hashlib.sha256).digest())
    return outs


def mix_hash(h: bytes, data: bytes) -> bytes:
    return hashlib.sha256(h + data).digest()


def nonce_bytes(n: int) -> bytes:
    return bytes(4) + n.to_bytes(8, "little")


def aead_encrypt(key: bytes, n: int, ad: bytes, pt: bytes) -> bytes:
    return ChaCha20Poly1305(key).encrypt(nonce_bytes(n), pt, ad)


def aead_decrypt(key: bytes, n: int, ad: bytes, ct: bytes) -> bytes:
    return ChaCha20Poly1305(key).decrypt(nonce_bytes(n), ct, ad)


def dh(priv: bytes, peer_pub: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(priv).exchange(
        X25519PublicKey.from_public_bytes(peer_pub)
    )


def pubkey(priv: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes_raw()


def clamp(seed: bytes) -> bytes:
    k = bytearray(seed)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    return bytes(k)


def hexs(b: bytes) -> str:
    return b.hex()


def main() -> None:
    client_priv = clamp(CLIENT_ENTROPY)
    server_priv = clamp(SERVER_ENTROPY)
    client_pub = pubkey(client_priv)
    server_pub = pubkey(server_priv)

    # ---- Initialize() -------------------------------------------------
    h = hashlib.sha256(PROTOCOL_NAME).digest()
    ck = h
    h = mix_hash(h, PROLOGUE)

    # ---- message 1: psk, e, payload -----------------------------------
    ck, temp_h, k = hkdf(ck, PSK, 3)
    h = mix_hash(h, temp_h)
    n = 0
    h = mix_hash(h, client_pub)
    ck, k = hkdf(ck, client_pub, 2)
    n = 0
    tag1 = aead_encrypt(k, n, h, b"")
    n += 1
    h = mix_hash(h, tag1)
    msg1 = client_pub + tag1

    # ---- message 2: e, ee, payload ------------------------------------
    h = mix_hash(h, server_pub)
    ck, k = hkdf(ck, server_pub, 2)
    n = 0
    ee = dh(server_priv, client_pub)
    ck, k = hkdf(ck, ee, 2)
    n = 0
    tag2 = aead_encrypt(k, n, h, b"")
    n += 1
    h = mix_hash(h, tag2)
    msg2 = server_pub + tag2

    # ---- Split() ------------------------------------------------------
    send_k, recv_k = hkdf(ck, b"", 2)
    handshake_hash = h

    # ---- transport sample ---------------------------------------------
    # ESPHome packet: [type_hi, type_lo, len_hi, len_lo, payload...]
    plaintext = bytes([0, 1, 0, 0])
    frame = aead_encrypt(send_k, 0, b"", plaintext)
    server_frame = aead_encrypt(recv_k, 0, b"", b"\x00\x06\x00\x00")

    print(f"prologue            = {hexs(PROLOGUE)}")
    print(f"psk                 = {hexs(PSK)}")
    print(f"client_entropy      = {hexs(CLIENT_ENTROPY)}")
    print(f"client_priv_clamped = {hexs(client_priv)}")
    print(f"client_pub          = {hexs(client_pub)}")
    print(f"server_entropy      = {hexs(SERVER_ENTROPY)}")
    print(f"server_priv_clamped = {hexs(server_priv)}")
    print(f"server_pub          = {hexs(server_pub)}")
    print(f"handshake_hash      = {hexs(handshake_hash)}")
    print(f"msg1                = {hexs(msg1)}")
    print(f"msg2                = {hexs(msg2)}")
    print(f"send_key            = {hexs(send_k)}")
    print(f"recv_key            = {hexs(recv_k)}")
    print(f"transport_frame     = {hexs(frame)}")
    print(f"server_frame        = {hexs(server_frame)}")
    print()
    print("/* --- C fixture --- */")
    for name, value in (
        ("PROLOGUE", PROLOGUE),
        ("PSK", PSK),
        ("CLIENT_ENTROPY", CLIENT_ENTROPY),
        ("SERVER_ENTROPY", SERVER_ENTROPY),
        ("CLIENT_PUB", client_pub),
        ("SERVER_PUB", server_pub),
        ("HANDSHAKE_HASH", handshake_hash),
        ("MSG1", msg1),
        ("MSG2", msg2),
        ("SEND_KEY", send_k),
        ("RECV_KEY", recv_k),
        ("TRANSPORT_FRAME", frame),
        ("SERVER_FRAME", server_frame),
    ):
        body = ", ".join(f"0x{b:02x}" for b in value)
        print(f"static const uint8_t {name}[{len(value)}] = {{ {body} }};")


if __name__ == "__main__":
    main()
