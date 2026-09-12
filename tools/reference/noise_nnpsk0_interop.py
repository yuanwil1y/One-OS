#!/usr/bin/env python3
"""Interoperability check for the One-OS Noise_NNpsk0 client.

Runs the handshake from the reference implementation for one fixed pair of
ephemeral keys and prints what each side must produce, so a single failing
implementation can be compared against it byte for byte:

  - message 1 (initiator -> responder) and the key material after the psk and
    e tokens;
  - the responder's message 2 and the AEAD parameters that produce it.

Usage:
  python tools/reference/noise_nnpsk0_interop.py <client_priv_hex> <server_priv_hex> [psk_hex]
"""

from __future__ import annotations

import hashlib
import hmac
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

PROTOCOL_NAME = b"Noise_NNpsk0_25519_ChaChaPoly_SHA256"
PROLOGUE = b"NoiseAPIInit"


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


def show(label: str, value: bytes) -> None:
    print(f"{label:<22} {value.hex()}")


def main() -> int:
    client_priv = clamp(bytes.fromhex(sys.argv[1]))
    server_priv = clamp(bytes.fromhex(sys.argv[2]))
    psk = bytes.fromhex(sys.argv[3]) if len(sys.argv) > 3 else bytes(32)
    client_pub = pubkey(client_priv)
    server_pub = pubkey(server_priv)

    h = hashlib.sha256(PROTOCOL_NAME).digest()
    ck = h
    h = mix_hash(h, PROLOGUE)

    # message 1: psk, e, payload
    ck, temp_h, k = hkdf(ck, psk, 3)
    h = mix_hash(h, temp_h)
    n = 0
    h = mix_hash(h, client_pub)
    ck, k = hkdf(ck, client_pub, 2)
    n = 0
    tag1 = ChaCha20Poly1305(k).encrypt(nonce_bytes(n), b"", h)
    n += 1
    h = mix_hash(h, tag1)

    print("== after psk and e (both sides must agree here) ==")
    show("ck", ck)
    show("k", k)
    show("h", h)
    show("msg1", client_pub + tag1)

    # message 2: e, ee, payload
    h = mix_hash(h, server_pub)
    ck, k = hkdf(ck, server_pub, 2)
    n = 0
    ee = dh(server_priv, client_pub)
    ck, k = hkdf(ck, ee, 2)
    n = 0

    print("== responder produces message 2 ==")
    show("ee", ee)
    show("ck", ck)
    show("k", k)
    show("h (associated data)", h)
    print(f"{'nonce':<22} {n}")
    tag2 = ChaCha20Poly1305(k).encrypt(nonce_bytes(n), b"", h)
    show("msg2", server_pub + tag2)

    send_k, recv_k = hkdf(ck, b"", 2)
    print("== transport keys ==")
    show("initiator send", send_k)
    show("initiator recv", recv_k)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
