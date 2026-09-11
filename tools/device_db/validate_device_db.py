#!/usr/bin/env python3
"""One-OS Device DB (.nbdb) host-side validator.

An independent check of a generated (or hand-modified) image. It re-reads the
bytes from scratch rather than trusting nbdb.py's writer, so a writer bug and a
reader bug have to agree to slip through.

Exit status is 0 for a valid file and 1 for any rejection, with a one-line
reason printed to stderr. `--expect-failure` inverts the exit status so a test
can assert that a deliberately corrupted fixture is rejected.
"""

from __future__ import annotations

import argparse
import dataclasses
import struct
import sys
from typing import List, Optional, Tuple

MAGIC = b"NBDB"
FORMAT_VERSION = 1
SCHEMA_VERSION = 1
READER_ABI = 1

HEADER_SIZE = 128
PROVENANCE_SIZE = 24
IDENTITY_SIZE = 24
PROFILE_SIZE = 96
FINGERPRINT_SIZE = 40
RECIPE_SIZE = 72
INDEX_BUCKET_SIZE = 32
INDEX_ENTRIES_PER_BUCKET = 3

MAX_FILE_BYTES = 16 * 1024 * 1024

POLICY_WRITE_CONTROL = 1 << 4
POLICY_USER_ACTION_REQUIRED = 1 << 3

PROTO_NAMES = ["ble", "wifi", "mdns", "ssdp", "lan", "zigbee", "matter", "esphome"]

NEVER_SAFE_IDENTITY = {7, 8, 9, 10, 11, 12, 13}
KNOWN_IDENTITY_MAX = 13

BACKEND_NONE = 0
BACKEND_PASSIVE_VALUE = 1
BACKEND_ZIGBEE_ATTRIBUTE = 4
BACKEND_ZIGBEE_COMMAND = 5
BACKEND_MATTER_ATTRIBUTE = 6
BACKEND_MATTER_COMMAND = 7

DOMAIN_MAX = 7
NO_INDEX = 0xFFFFFFFF
NO_ENDPOINT = 0xFFFF


class Invalid(Exception):
    """The file was rejected; the message is the reason."""


def crc32_ieee(data: bytes) -> int:
    """CRC-32 IEEE, matching device_db_crc32() in the firmware reader.

    zlib.crc32 already applies the init value and the final xor. The mask is
    explicit because zlib may return a signed value on some builds.
    """
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


def fnv1a32(data: bytes) -> int:
    h = 2166136261
    for byte in data:
        h ^= byte
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def normalize_key(text: str) -> bytes:
    """Canonical fingerprint key bytes.

    The record stores the human-readable declaration, but the hash covers the
    canonical form: lowercase with separators removed. The generator defines that
    form, and the validator must apply the same rule or it would compute a
    different hash for keys such as 'example|plug-zb-2'.
    """
    cleaned = text.strip().lower().replace(":", "").replace("-", "")
    return cleaned.encode("utf-8")


@dataclasses.dataclass
class Info:
    format_version: int
    schema_version: int
    reader_abi: int
    file_length: int
    content_version: int
    build_timestamp: int
    profile_count: int
    fingerprint_count: int
    recipe_count: int
    identity_count: int
    provenance_count: int
    index_bucket_count: int
    strings_offset: int
    strings_length: int
    provenance_offset: int
    identity_offset: int
    profiles_offset: int
    fingerprints_offset: int
    recipes_offset: int
    index_offset: int
    index_length: int


class Database:
    """A validated image. Only constructed by validate()."""

    def __init__(self, data: bytes, info: Info) -> None:
        self.data = data
        self.info = info
        self.strings = data[info.strings_offset:info.strings_offset + info.strings_length]

    # -- string refs -------------------------------------------------------

    def string(self, ref: int, *, required: bool = False) -> str:
        length = ref & 0xFF
        offset = ref >> 8
        if length == 0:
            if required:
                raise Invalid("required string is absent")
            return ""
        if offset + length > len(self.strings):
            raise Invalid(
                f"string ref (offset={offset}, len={length}) leaves the string table"
            )
        return self.strings[offset:offset + length].decode("utf-8", errors="replace")

    # -- records -----------------------------------------------------------

    def profiles(self):
        for i in range(self.info.profile_count):
            base = self.info.profiles_offset + i * PROFILE_SIZE
            p = self.data[base:base + PROFILE_SIZE]
            yield {
                "index": i,
                "profile_id": struct.unpack_from("<I", p, 0)[0],
                "vendor": self.string(struct.unpack_from("<I", p, 4)[0]),
                "model": self.string(struct.unpack_from("<I", p, 8)[0]),
                "display_name": self.string(struct.unpack_from("<I", p, 12)[0]),
                "icon": self.string(struct.unpack_from("<I", p, 16)[0]),
                "provenance_index": struct.unpack_from("<I", p, 20)[0],
                "theengs_decoder_id": struct.unpack_from("<I", p, 24)[0],
                "zha_quirk_id": struct.unpack_from("<I", p, 28)[0],
                "first_recipe": struct.unpack_from("<I", p, 32)[0],
                "recipe_count": struct.unpack_from("<I", p, 36)[0],
                "policy": struct.unpack_from("<I", p, 40)[0],
                "identity_first": struct.unpack_from("<I", p, 44)[0],
                "identity_count": struct.unpack_from("<I", p, 48)[0],
                "protocol_mask": p[52],
                "writable": p[53],
                "fingerprint_first": struct.unpack_from("<I", p, 56)[0],
                "fingerprint_count": struct.unpack_from("<I", p, 60)[0],
            }

    def fingerprints(self):
        for i in range(self.info.fingerprint_count):
            base = self.info.fingerprints_offset + i * FINGERPRINT_SIZE
            f = self.data[base:base + FINGERPRINT_SIZE]
            key_hash = struct.unpack_from("<I", f, 4)[0]
            key = self.string(struct.unpack_from("<I", f, 8)[0], required=True)
            yield {
                "index": i,
                "protocol": f[0],
                "match_kind": f[1],
                "key_hash": key_hash,
                "key": key,
                "profile_id": struct.unpack_from("<I", f, 12)[0],
                "mask_length": struct.unpack_from("<I", f, 16)[0],
                "mask": self.string(struct.unpack_from("<I", f, 20)[0]),
                "flags": struct.unpack_from("<I", f, 28)[0],
            }

    def recipes(self):
        for i in range(self.info.recipe_count):
            base = self.info.recipes_offset + i * RECIPE_SIZE
            r = self.data[base:base + RECIPE_SIZE]
            yield {
                "index": i,
                "profile_id": struct.unpack_from("<I", r, 0)[0],
                "domain": self.string(struct.unpack_from("<I", r, 4)[0], required=True),
                "name": self.string(struct.unpack_from("<I", r, 8)[0]),
                "device_class": self.string(struct.unpack_from("<I", r, 12)[0]),
                "unit": self.string(struct.unpack_from("<I", r, 16)[0]),
                "domain_id": r[20],
                "backend": r[21],
                "read_source_id": struct.unpack_from("<I", r, 24)[0],
                "write_target_id": struct.unpack_from("<I", r, 28)[0],
                "codec_id": struct.unpack_from("<I", r, 32)[0],
                "subscription_id": struct.unpack_from("<I", r, 36)[0],
                "endpoint": struct.unpack_from("<H", r, 40)[0],
                "cluster": struct.unpack_from("<H", r, 42)[0],
                "attribute": struct.unpack_from("<H", r, 44)[0],
                "command": struct.unpack_from("<H", r, 46)[0],
                "min_value": struct.unpack_from("<i", r, 48)[0],
                "max_value": struct.unpack_from("<i", r, 52)[0],
                "scale": struct.unpack_from("<I", r, 56)[0],
                "flags": struct.unpack_from("<I", r, 60)[0],
            }

    def identities(self):
        for i in range(self.info.identity_count):
            base = self.info.identity_offset + i * IDENTITY_SIZE
            r = self.data[base:base + IDENTITY_SIZE]
            yield {
                "index": i,
                "kind": r[0],
                "strength": r[1],
                "key": self.string(struct.unpack_from("<I", r, 4)[0]),
                "profile_id": struct.unpack_from("<I", r, 8)[0],
                "flags": struct.unpack_from("<I", r, 12)[0],
            }

    def provenances(self):
        for i in range(self.info.provenance_count):
            base = self.info.provenance_offset + i * PROVENANCE_SIZE
            r = self.data[base:base + PROVENANCE_SIZE]
            yield {
                "index": i,
                "source_id": struct.unpack_from("<I", r, 0)[0],
                "reuse": r[4],
                "license": r[5],
                "source_revision": struct.unpack_from("<I", r, 8)[0],
                "source_name": self.string(struct.unpack_from("<I", r, 12)[0]),
                "review": self.string(struct.unpack_from("<I", r, 16)[0]),
            }


def _range_ok(base: int, limit: int, offset: int, length: int) -> bool:
    return offset >= base and offset + length <= limit


def validate(data: bytes) -> Database:
    """Validate an image and return a Database, or raise Invalid with a reason."""

    size = len(data)
    if size < HEADER_SIZE:
        raise Invalid(f"truncated: {size} bytes is smaller than the 128-byte header")
    if size > MAX_FILE_BYTES:
        raise Invalid(f"too large: {size} exceeds the 16 MiB cap")
    if data[0:4] != MAGIC:
        raise Invalid(f"not a device db: magic is {data[0:4]!r}")

    fmt, schema = struct.unpack_from("<HH", data, 4)
    if fmt != FORMAT_VERSION or schema != SCHEMA_VERSION:
        raise Invalid(f"incompatible version: format={fmt} schema={schema}")
    abi = struct.unpack_from("<I", data, 8)[0]
    if abi > READER_ABI:
        raise Invalid(f"incompatible reader ABI: file needs {abi}, have {READER_ABI}")
    flags = struct.unpack_from("<I", data, 12)[0]
    if flags != 0:
        raise Invalid(f"reserved header flags are set: 0x{flags:08x}")

    file_length = struct.unpack_from("<I", data, 16)[0]
    if file_length != size:
        raise Invalid(
            f"file_length says {file_length} but the file is {size} bytes"
        )

    header_crc = struct.unpack_from("<I", data, 120)[0]
    if crc32_ieee(data[0:120]) != header_crc:
        raise Invalid("header_crc32 mismatch")
    body_crc = struct.unpack_from("<I", data, 116)[0]
    if crc32_ieee(data[HEADER_SIZE:]) != body_crc:
        raise Invalid("body_crc32 mismatch")

    bucket_count = struct.unpack_from("<H", data, 52)[0]
    if bucket_count and (bucket_count & (bucket_count - 1)):
        raise Invalid(f"index_bucket_count {bucket_count} is not a power of two")

    info = Info(
        format_version=fmt,
        schema_version=schema,
        reader_abi=abi,
        file_length=file_length,
        content_version=struct.unpack_from("<I", data, 20)[0],
        build_timestamp=struct.unpack_from("<Q", data, 24)[0],
        profile_count=struct.unpack_from("<I", data, 32)[0],
        fingerprint_count=struct.unpack_from("<I", data, 36)[0],
        recipe_count=struct.unpack_from("<I", data, 40)[0],
        identity_count=struct.unpack_from("<I", data, 44)[0],
        provenance_count=struct.unpack_from("<I", data, 48)[0],
        index_bucket_count=bucket_count,
        strings_offset=struct.unpack_from("<I", data, 56)[0],
        strings_length=struct.unpack_from("<I", data, 60)[0],
        provenance_offset=struct.unpack_from("<I", data, 64)[0],
        identity_offset=struct.unpack_from("<I", data, 72)[0],
        profiles_offset=struct.unpack_from("<I", data, 80)[0],
        fingerprints_offset=struct.unpack_from("<I", data, 88)[0],
        recipes_offset=struct.unpack_from("<I", data, 96)[0],
        index_offset=struct.unpack_from("<I", data, 104)[0],
        index_length=struct.unpack_from("<I", data, 108)[0],
    )

    # Region table: (offset, length, record size, count, name)
    regions = [
        (info.strings_offset, info.strings_length, 0, 0, "strings"),
        (info.provenance_offset, struct.unpack_from("<I", data, 68)[0],
         PROVENANCE_SIZE, info.provenance_count, "provenance"),
        (info.identity_offset, struct.unpack_from("<I", data, 76)[0],
         IDENTITY_SIZE, info.identity_count, "identity"),
        (info.profiles_offset, struct.unpack_from("<I", data, 84)[0],
         PROFILE_SIZE, info.profile_count, "profiles"),
        (info.fingerprints_offset, struct.unpack_from("<I", data, 92)[0],
         FINGERPRINT_SIZE, info.fingerprint_count, "fingerprints"),
        (info.recipes_offset, struct.unpack_from("<I", data, 100)[0],
         RECIPE_SIZE, info.recipe_count, "recipes"),
    ]
    for offset, length, record_size, count, name in regions:
        if not _range_ok(HEADER_SIZE, size, offset, length):
            raise Invalid(
                f"{name} region [{offset}, {offset + length}) lies outside the file"
            )
        if record_size:
            if record_size * count != length:
                raise Invalid(
                    f"{name} region length {length} does not equal "
                    f"{record_size} * {count}"
                )
        elif count:
            raise Invalid(f"{name} region is variable-length but declares a count")

    if not _range_ok(HEADER_SIZE, size, info.index_offset, info.index_length):
        raise Invalid("index region lies outside the file")
    if info.index_bucket_count:
        bucket_bytes = info.index_bucket_count * INDEX_BUCKET_SIZE
        if bucket_bytes > info.index_length:
            raise Invalid("index region is shorter than its bucket array")

    db = Database(data, info)

    # Materialise the record lists once. The checks below cross-reference records
    # repeatedly, and re-iterating a generator per lookup would be quadratic.
    profiles = list(db.profiles())
    recipes = list(db.recipes())
    fingerprints = list(db.fingerprints())
    identities = list(db.identities())

    # ---- profiles --------------------------------------------------------
    profile_ids: List[int] = []
    previous_id = 0
    for profile in profiles:
        pid = profile["profile_id"]
        if pid == 0:
            raise Invalid("profile_id 0 is reserved")
        if pid <= previous_id:
            raise Invalid(
                f"profile ids must ascend without duplicates (saw {pid} after "
                f"{previous_id})"
            )
        previous_id = pid
        profile_ids.append(pid)

        if profile["provenance_index"] >= info.provenance_count:
            raise Invalid(f"profile {pid} provenance_index out of range")
        if profile["first_recipe"] + profile["recipe_count"] > info.recipe_count:
            raise Invalid(f"profile {pid} recipe range out of bounds")
        if profile["identity_first"] + profile["identity_count"] > info.identity_count:
            raise Invalid(f"profile {pid} identity range out of bounds")
        if (profile["fingerprint_first"] + profile["fingerprint_count"]
                > info.fingerprint_count):
            raise Invalid(f"profile {pid} fingerprint range out of bounds")
        if profile["fingerprint_count"] == 0:
            raise Invalid(f"profile {pid} has no fingerprint and could never match")
        if profile["protocol_mask"] == 0:
            raise Invalid(f"profile {pid} has an empty protocol_mask")
        if profile["writable"] not in (0, 1):
            raise Invalid(f"profile {pid} writable is not a boolean")
        if (profile["policy"] & POLICY_WRITE_CONTROL) and not (
                profile["policy"] & POLICY_USER_ACTION_REQUIRED):
            raise Invalid(
                f"profile {pid} declares write_control without user_action_required"
            )

        if profile["writable"] == 1:
            writable = False
            for r in range(profile["recipe_count"]):
                recipe = recipes[profile["first_recipe"] + r]
                if recipe["backend"] not in (BACKEND_NONE, BACKEND_PASSIVE_VALUE):
                    writable = True
                    break
            if not writable:
                raise Invalid(f"writable profile {pid} has no writable recipe")

    profile_id_set = set(profile_ids)

    # ---- recipes ---------------------------------------------------------
    for recipe in recipes:
        if recipe["profile_id"] not in profile_id_set:
            raise Invalid(
                f"recipe {recipe['index']} references unknown profile "
                f"{recipe['profile_id']}"
            )
        if recipe["domain_id"] > DOMAIN_MAX:
            raise Invalid(f"recipe {recipe['index']} domain_id out of range")
        if recipe["backend"] > BACKEND_MATTER_COMMAND:
            raise Invalid(f"recipe {recipe['index']} backend out of range")
        if recipe["min_value"] > recipe["max_value"]:
            raise Invalid(f"recipe {recipe['index']} min_value exceeds max_value")

        backend = recipe["backend"]
        if backend == BACKEND_NONE and recipe["write_target_id"] != NO_INDEX:
            raise Invalid(f"recipe {recipe['index']}: 'none' backend is writable")
        if backend == BACKEND_PASSIVE_VALUE and recipe["write_target_id"] != NO_INDEX:
            raise Invalid(f"recipe {recipe['index']}: passive_value is writable")
        if backend in (BACKEND_ZIGBEE_ATTRIBUTE, BACKEND_ZIGBEE_COMMAND):
            if recipe["endpoint"] == NO_ENDPOINT or recipe["cluster"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: zigbee needs endpoint+cluster")
            if backend == BACKEND_ZIGBEE_COMMAND and recipe["command"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: zigbee command missing id")
            if backend == BACKEND_ZIGBEE_ATTRIBUTE and recipe["attribute"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: zigbee attribute missing id")
        if backend in (BACKEND_MATTER_ATTRIBUTE, BACKEND_MATTER_COMMAND):
            if recipe["endpoint"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: matter needs an endpoint")
            if backend == BACKEND_MATTER_COMMAND and recipe["command"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: matter command missing id")
            if backend == BACKEND_MATTER_ATTRIBUTE and recipe["attribute"] == NO_ENDPOINT:
                raise Invalid(f"recipe {recipe['index']}: matter attribute missing id")

    # ---- fingerprints ----------------------------------------------------
    for fp in fingerprints:
        if fp["profile_id"] not in profile_id_set:
            raise Invalid(
                f"fingerprint {fp['index']} references unknown profile "
                f"{fp['profile_id']}"
            )
        if fp["protocol"] >= len(PROTO_NAMES):
            raise Invalid(f"fingerprint {fp['index']} protocol out of range")
        if fp["match_kind"] > 2:
            raise Invalid(f"fingerprint {fp['index']} match_kind out of range")
        if not fp["key"]:
            raise Invalid(f"fingerprint {fp['index']} has an empty key")
        if fnv1a32(normalize_key(fp["key"])) != fp["key_hash"]:
            raise Invalid(
                f"fingerprint {fp['index']} key_hash does not match its key"
            )
        if fp["match_kind"] == 2:
            if not fp["mask"]:
                raise Invalid(f"fingerprint {fp['index']} bitmask has no mask")
            if fp["mask_length"] != len(fp["mask"].encode("utf-8")):
                raise Invalid(
                    f"fingerprint {fp['index']} mask_length disagrees with the mask"
                )

    # ---- identity rules --------------------------------------------------
    for rule in identities:
        if rule["kind"] > KNOWN_IDENTITY_MAX:
            raise Invalid(f"identity {rule['index']} kind out of range")
        if rule["strength"] > 2:
            raise Invalid(f"identity {rule['index']} strength out of range")
        if rule["kind"] in NEVER_SAFE_IDENTITY and rule["strength"] != 0:
            raise Invalid(
                f"identity {rule['index']} uses kind {rule['kind']} which may never "
                f"merge devices, but declares a usable strength"
            )
        if rule["kind"] == 0 and rule["strength"] != 0:
            raise Invalid(f"identity {rule['index']} kind 'none' must be unsafe")
        if rule["profile_id"] not in profile_id_set:
            raise Invalid(
                f"identity {rule['index']} references unknown profile "
                f"{rule['profile_id']}"
            )

    # ---- index -----------------------------------------------------------
    if info.index_bucket_count:
        overflow_bytes = info.index_length - info.index_bucket_count * INDEX_BUCKET_SIZE
        if overflow_bytes % 10 != 0:
            raise Invalid("index overflow run is not entry-aligned")
        overflow_entries = overflow_bytes // 10
        flagged = 0
        for b in range(info.index_bucket_count):
            base = info.index_offset + b * INDEX_BUCKET_SIZE
            count = struct.unpack_from("<H", data, base)[0]
            flags = struct.unpack_from("<H", data, base + 30)[0]
            if count > INDEX_ENTRIES_PER_BUCKET:
                raise Invalid(f"bucket {b} declares {count} entries")
            if flags & ~1:
                raise Invalid(f"bucket {b} has unknown flag bits 0x{flags:04x}")
            if flags & 1:
                flagged += 1
            for e in range(count):
                fp_index = struct.unpack_from("<I", data, base + 2 + e * 10 + 4)[0]
                if fp_index >= info.fingerprint_count:
                    raise Invalid(f"bucket {b} entry {e} points outside fingerprints")
        if flagged and overflow_entries == 0:
            raise Invalid("a bucket marks overflow but the overflow run is empty")
        if not flagged and overflow_entries:
            raise Invalid("overflow run has entries but no bucket is flagged")

    return db


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="the .nbdb file to validate")
    parser.add_argument("--expect-failure", action="store_true",
                        help="exit 0 only if the file is REJECTED")
    parser.add_argument("--summary", action="store_true",
                        help="print a summary of a valid file")
    args = parser.parse_args()

    try:
        with open(args.path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        print(f"cannot read {args.path}: {exc}", file=sys.stderr)
        return 1

    try:
        db = validate(data)
    except Invalid as exc:
        if args.expect_failure:
            print(f"rejected as expected: {exc}")
            return 0
        print(f"INVALID {args.path}: {exc}", file=sys.stderr)
        return 1

    if args.expect_failure:
        print(f"UNEXPECTEDLY VALID {args.path}", file=sys.stderr)
        return 1

    print(f"valid {args.path}: {len(data)} bytes")
    if args.summary:
        info = db.info
        print(f"  content_version   {info.content_version}")
        print(f"  build_timestamp   {info.build_timestamp}")
        print(f"  profiles          {info.profile_count}")
        print(f"  fingerprints      {info.fingerprint_count}")
        print(f"  recipes           {info.recipe_count}")
        print(f"  identity rules    {info.identity_count}")
        print(f"  provenance        {info.provenance_count}")
        print(f"  index buckets     {info.index_bucket_count}")
        for profile in db.profiles():
            protocols = [n for i, n in enumerate(PROTO_NAMES)
                         if profile["protocol_mask"] & (1 << i)]
            print(f"  profile {profile['profile_id']}: {profile['display_name']!r} "
                  f"model={profile['model']!r} protocols={','.join(protocols)} "
                  f"recipes={profile['recipe_count']} writable={profile['writable']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
