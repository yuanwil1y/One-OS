#!/usr/bin/env python3
"""One-OS Device DB (.nbdb) container writer.

This module implements exactly the layout in docs/device-db-format.md. It is the
generator side of the contract; the reader side is
firmware/main/device_db_format.c, and the two must agree byte for byte.

Everything is written with explicit little-endian packing. Struct layout is never
part of the format, so no Python struct or C struct is ever memcpy'd into the
file.

Design constraints that come from the product rules:
  - the generated file must be byte-identical for the same input (see
    build_device_db.py, which is checked against a recorded digest);
  - a profile that only cites REFERENCE_ONLY provenance is rejected, not written;
  - the index never drops an entry: an overflowing bucket is marked and its extra
    entries are appended to a contiguous overflow run.
"""

from __future__ import annotations

import dataclasses
import hashlib
import struct
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

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
INDEX_ENTRY_SIZE = 10
INDEX_FLAG_OVERFLOW = 0x0001

MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_STRING_BYTES = 255  # a StringRef packs its length into 8 bits

NO_INDEX = 0xFFFFFFFF
NO_ENDPOINT = 0xFFFF

# --- enumerations (must match device_db_format.h) -------------------------

PROTOCOLS = {
    "ble": 0,
    "wifi": 1,
    "mdns": 2,
    "ssdp": 3,
    "lan": 4,
    "zigbee": 5,
    "matter": 6,
    "esphome": 7,
}

REUSE = {
    "copy": 0,
    "port": 1,
    "clean_room": 2,
    "reference_only": 3,
}

LICENSES = {
    "unknown": 0,
    "mit": 1,
    "bsd_2": 2,
    "bsd_3": 3,
    "apache_2": 4,
    "cc0": 5,
    "public_domain": 6,
    "proprietary_clean_room": 7,
}

IDENTITY_KINDS = {
    "none": 0,
    "zigbee_ieee": 1,
    "matter_node_fabric": 2,
    "vendor_protocol_uuid": 3,
    "esphome_node_name": 4,
    "mdns_txt_identifier": 5,
    "ble_public_address": 6,
    "ble_random_address": 7,
    "wifi_bssid": 8,
    "ip_address": 9,
    "rssi": 10,
    "ssid": 11,
    "model_name": 12,
    "matter_vid_pid": 13,
}

# Identity kinds that may never be used to merge devices across protocols.
NEVER_SAFE_IDENTITY = {
    "ble_random_address",
    "wifi_bssid",
    "ip_address",
    "rssi",
    "ssid",
    "model_name",
    "matter_vid_pid",
}

STRENGTH = {"unsafe": 0, "weak": 1, "strong": 2}

BACKENDS = {
    "none": 0,
    "passive_value": 1,
    "ble_gatt": 2,
    "esphome_api": 3,
    "zigbee_attribute": 4,
    "zigbee_command": 5,
    "matter_attribute": 6,
    "matter_command": 7,
}

DOMAINS = {
    "sensor": 0,
    "binary_sensor": 1,
    "switch": 2,
    "light": 3,
    "button": 4,
    "number": 5,
    "select": 6,
    "climate": 7,
}

POLICY = {
    "passive_only": 1 << 0,
    "safe_read": 1 << 1,
    "auth_required": 1 << 2,
    "user_action_required": 1 << 3,
    "write_control": 1 << 4,
}


class DeviceDbError(Exception):
    """A generator-side rule was violated; the file is not written."""


# --- helpers --------------------------------------------------------------


def crc32_ieee(data: bytes) -> int:
    """CRC-32 IEEE, matching device_db_crc32() in the firmware reader.

    zlib.crc32 already applies init 0xFFFFFFFF and the final xor, so the value is
    used as-is. Note the parentheses are not decoration: `&` binds tighter than
    `^` in Python, so an unparenthesised expression here silently produced a
    different (wrong) checksum.
    """
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF


def fnv1a32(data: bytes) -> int:
    """FNV-1a 32, matching device_db_key_hash() in the firmware reader."""
    h = 2166136261
    for byte in data:
        h ^= byte
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def normalize_key(text: str) -> bytes:
    """Canonical fingerprint key bytes.

    Lowercase, separators stripped. The generator defines the canonical form per
    protocol; the firmware compares the hash first and then the bytes, so this
    must be applied before hashing.
    """
    cleaned = text.strip().lower().replace(":", "").replace("-", "")
    return cleaned.encode("utf-8")


# --- string table ---------------------------------------------------------


class StringTable:
    """Deduplicating string pool. Offsets are assigned in first-use order so the
    output is deterministic for a given input ordering."""

    def __init__(self) -> None:
        self._data = bytearray()
        self._offsets: Dict[bytes, int] = {}

    def add(self, text: str) -> int:
        raw = text.encode("utf-8")
        if len(raw) > MAX_STRING_BYTES:
            raise DeviceDbError(f"string exceeds {MAX_STRING_BYTES} bytes: {text!r}")
        if len(raw) == 0:
            return 0  # the zero ref means "absent"
        if raw in self._offsets:
            return self._offsets[raw]
        offset = len(self._data)
        if offset > 0xFFFFFF:
            raise DeviceDbError("string table exceeds the 24-bit offset space")
        self._data.extend(raw)
        self._offsets[raw] = offset
        return offset

    def ref(self, text: Optional[str]) -> int:
        """Pack (offset, length) into a u32 cell. Zero means absent."""
        if text is None or text == "":
            return 0
        offset = self.add(text)
        if offset == 0:
            # Offset 0 is legal only for the first string; the zero *ref* is
            # reserved for "absent", so shift the table by one byte.
            raise DeviceDbError("internal: string at offset 0 must be reserved")
        length = len(text.encode("utf-8"))
        return (offset << 8) | length

    @property
    def data(self) -> bytes:
        return bytes(self._data)


# --- input model ----------------------------------------------------------


@dataclasses.dataclass
class Provenance:
    source_id: int
    reuse: str
    license: str
    source_revision: str
    source_name: str
    review: str


@dataclasses.dataclass
class IdentityRule:
    kind: str
    strength: str
    key: str = ""
    flags: int = 0


@dataclasses.dataclass
class Fingerprint:
    protocol: str
    key: str
    match_kind: str = "exact"
    mask: str = ""
    flags: int = 0


@dataclasses.dataclass
class Recipe:
    domain: str
    name: str = ""
    device_class: str = ""
    unit: str = ""
    backend: str = "none"
    read_source_id: int = NO_INDEX
    write_target_id: int = NO_INDEX
    codec_id: int = NO_INDEX
    subscription_id: int = NO_INDEX
    endpoint: int = NO_ENDPOINT
    cluster: int = NO_ENDPOINT
    attribute: int = NO_ENDPOINT
    command: int = NO_ENDPOINT
    min_value: int = 0
    max_value: int = 0
    scale: int = 0
    flags: int = 0


@dataclasses.dataclass
class Profile:
    profile_id: int
    vendor: str
    model: str
    display_name: str
    icon: str
    provenance: Provenance
    fingerprints: List[Fingerprint]
    recipes: List[Recipe] = dataclasses.field(default_factory=list)
    identity_rules: List[IdentityRule] = dataclasses.field(default_factory=list)
    theengs_decoder_id: int = NO_INDEX
    zha_quirk_id: int = NO_INDEX
    policy: Sequence[str] = ()
    writable: bool = False


# --- validation (generator side) -----------------------------------------


def _validate_profile(profile: Profile, known_source_ids: Iterable[int],
                      reference_only_sources: Iterable[int]) -> None:
    if profile.profile_id == 0:
        raise DeviceDbError("profile_id 0 is reserved")
    if not profile.fingerprints:
        raise DeviceDbError(
            f"profile {profile.profile_id} has no fingerprint and could never match"
        )
    if not profile.provenance:
        raise DeviceDbError(f"profile {profile.profile_id} has no provenance")

    provenance = profile.provenance
    known = set(known_source_ids)
    if provenance.source_id not in known:
        raise DeviceDbError(
            f"profile {profile.profile_id} cites source_id "
            f"{provenance.source_id} which is not in the source manifest"
        )

    # The provenance gate: a profile whose only justification is reference-only
    # material must not ship.
    if (provenance.reuse == "reference_only"
            or provenance.source_id in set(reference_only_sources)):
        raise DeviceDbError(
            f"profile {profile.profile_id} is justified only by REFERENCE_ONLY "
            f"source {provenance.source_id}; an independent specification, owned "
            f"capture or clean-room derivation is required before shipping"
        )

    if provenance.reuse not in REUSE:
        raise DeviceDbError(f"unknown reuse kind {provenance.reuse!r}")
    if provenance.license not in LICENSES:
        raise DeviceDbError(f"unknown license {provenance.license!r}")

    for fp in profile.fingerprints:
        if fp.protocol not in PROTOCOLS:
            raise DeviceDbError(f"unknown protocol {fp.protocol!r}")
        if fp.match_kind not in ("exact", "prefix", "bitmask"):
            raise DeviceDbError(f"unknown match_kind {fp.match_kind!r}")
        if not normalize_key(fp.key):
            raise DeviceDbError(
                f"profile {profile.profile_id} has an empty {fp.protocol} key"
            )
        if fp.match_kind == "bitmask" and not fp.mask:
            raise DeviceDbError(
                f"profile {profile.profile_id} bitmask fingerprint has no mask"
            )

    policy = 0
    for name in profile.policy:
        if name not in POLICY:
            raise DeviceDbError(f"unknown policy flag {name!r}")
        policy |= POLICY[name]
    # A state-changing operation always requires explicit user action.
    if (policy & POLICY["write_control"]) and not (policy & POLICY["user_action_required"]):
        raise DeviceDbError(
            f"profile {profile.profile_id} declares write_control without "
            f"user_action_required"
        )

    if profile.writable:
        writable_backends = {"ble_gatt", "esphome_api", "zigbee_attribute",
                             "zigbee_command", "matter_attribute", "matter_command"}
        if not any(r.backend in writable_backends for r in profile.recipes):
            raise DeviceDbError(
                f"profile {profile.profile_id} is marked writable but has no "
                f"writable recipe"
            )

    for rule in profile.identity_rules:
        if rule.kind not in IDENTITY_KINDS:
            raise DeviceDbError(f"unknown identity kind {rule.kind!r}")
        if rule.strength not in STRENGTH:
            raise DeviceDbError(f"unknown identity strength {rule.strength!r}")
        if rule.kind in NEVER_SAFE_IDENTITY and rule.strength != "unsafe":
            raise DeviceDbError(
                f"identity kind {rule.kind!r} may never merge devices; it must be "
                f"declared unsafe (profile {profile.profile_id})"
            )
        if rule.kind == "none" and rule.strength != "unsafe":
            raise DeviceDbError("identity kind 'none' must be declared unsafe")

    for recipe in profile.recipes:
        _validate_recipe(profile.profile_id, recipe)


def _validate_recipe(profile_id: int, recipe: Recipe) -> None:
    if recipe.domain not in DOMAINS:
        raise DeviceDbError(f"profile {profile_id}: unknown domain {recipe.domain!r}")
    if recipe.backend not in BACKENDS:
        raise DeviceDbError(f"profile {profile_id}: unknown backend {recipe.backend!r}")

    if recipe.backend == "none" and recipe.write_target_id != NO_INDEX:
        raise DeviceDbError(
            f"profile {profile_id}: a 'none' recipe must not set write_target_id"
        )
    if recipe.backend == "passive_value" and recipe.write_target_id != NO_INDEX:
        raise DeviceDbError(
            f"profile {profile_id}: a passive_value recipe must not be writable"
        )
    if recipe.backend in ("zigbee_attribute", "zigbee_command"):
        if recipe.endpoint == NO_ENDPOINT or recipe.cluster == NO_ENDPOINT:
            raise DeviceDbError(
                f"profile {profile_id}: zigbee recipe needs endpoint and cluster"
            )
        if recipe.backend == "zigbee_command" and recipe.command == NO_ENDPOINT:
            raise DeviceDbError(f"profile {profile_id}: zigbee command needs a command id")
        if recipe.backend == "zigbee_attribute" and recipe.attribute == NO_ENDPOINT:
            raise DeviceDbError(
                f"profile {profile_id}: zigbee attribute needs an attribute id"
            )
    if recipe.backend in ("matter_attribute", "matter_command"):
        if recipe.endpoint == NO_ENDPOINT:
            raise DeviceDbError(f"profile {profile_id}: matter recipe needs an endpoint")
        if recipe.backend == "matter_command" and recipe.command == NO_ENDPOINT:
            raise DeviceDbError(f"profile {profile_id}: matter command needs a command id")
        if recipe.backend == "matter_attribute" and recipe.attribute == NO_ENDPOINT:
            raise DeviceDbError(
                f"profile {profile_id}: matter attribute needs an attribute id"
            )
    if recipe.min_value > recipe.max_value:
        raise DeviceDbError(
            f"profile {profile_id}: min_value exceeds max_value"
        )


# --- writer ---------------------------------------------------------------


def build(profiles: Sequence[Profile], *, content_version: int,
          build_timestamp: int = 0,
          known_source_ids: Iterable[int] = (),
          reference_only_sources: Iterable[int] = (),
          index_bucket_count: Optional[int] = None) -> bytes:
    """Serialize profiles into an .nbdb image.

    Deterministic: identical input yields identical bytes. Profiles are sorted by
    profile_id and fingerprints by (protocol, key_hash, profile_id).
    """
    ordered = sorted(profiles, key=lambda p: p.profile_id)
    seen_ids = set()
    for profile in ordered:
        _validate_profile(profile, known_source_ids, reference_only_sources)
        if profile.profile_id in seen_ids:
            raise DeviceDbError(f"duplicate profile_id {profile.profile_id}")
        seen_ids.add(profile.profile_id)

    strings = StringTable()
    # Reserve "offset 0" so a zero ref is unambiguously "absent".
    _reserve = strings.add(" ")  # a single space occupies offset 0
    assert _reserve == 0

    # --- provenance and identity records, in profile order ---------------
    provenance_records: List[bytes] = []
    identity_records: List[bytes] = []
    identity_first: Dict[int, Tuple[int, int]] = {}

    for profile in ordered:
        prov = profile.provenance
        provenance_records.append(
            struct.pack(
                "<IBBHIII",
                prov.source_id,
                REUSE[prov.reuse],
                LICENSES[prov.license],
                0,
                int(prov.source_revision, 0) if prov.source_revision else 0,
                strings.ref(prov.source_name),
                strings.ref(prov.review),
            )
            + b"\x00" * 4
        )
        assert len(provenance_records[-1]) == PROVENANCE_SIZE

        first = len(identity_records)
        for rule in profile.identity_rules:
            identity_records.append(
                struct.pack(
                    "<BBHIIII",
                    IDENTITY_KINDS[rule.kind],
                    STRENGTH[rule.strength],
                    0,
                    strings.ref(rule.key) if rule.key else 0,
                    profile.profile_id,
                    rule.flags,
                    0,
                )
                + b"\x00" * 4
            )
            assert len(identity_records[-1]) == IDENTITY_SIZE
        identity_first[profile.profile_id] = (first, len(identity_records) - first)

    # --- profiles (header part) -----------------------------------------
    # Fingerprints and recipes are laid out per profile, in profile order, which
    # keeps each profile's range contiguous and easy to validate.
    fingerprint_records: List[bytes] = []
    fingerprint_ranges: Dict[int, Tuple[int, int]] = {}
    recipe_records: List[bytes] = []
    recipe_ranges: Dict[int, Tuple[int, int]] = {}

    for profile in ordered:
        # Sort this profile's fingerprints for determinism, then append.
        fps = sorted(
            profile.fingerprints,
            key=lambda f: (PROTOCOLS[f.protocol], fnv1a32(normalize_key(f.key))),
        )
        first_fp = len(fingerprint_records)
        for fp in fps:
            key_bytes = normalize_key(fp.key)
            match_kind = {"exact": 0, "prefix": 1, "bitmask": 2}[fp.match_kind]
            mask_ref = strings.ref(fp.mask) if fp.mask else 0
            mask_len = len(fp.mask.encode("utf-8")) if fp.mask else 0
            # Layout from the spec, offsets in parentheses:
            #   protocol(0) match_kind(1) pad(2..3) key_hash(4) key_ref(8)
            #   profile_id(12) mask_length(16) mask_ref(20) flags(28) pad(32..39)
            # Fields occupy 28 bytes; the record is 40, so 12 bytes of zero pad.
            packed = (
                bytes([PROTOCOLS[fp.protocol], match_kind])
                + b"\x00\x00"
                + struct.pack("<I", fnv1a32(key_bytes))
                + struct.pack("<I", strings.ref(fp.key))
                + struct.pack("<I", profile.profile_id)
                + struct.pack("<I", mask_len)
                + struct.pack("<I", mask_ref)
                + struct.pack("<I", fp.flags)
            )
            assert len(packed) == 28, f"fingerprint layout is {len(packed)} bytes"
            fingerprint_records.append(packed + b"\x00" * (FINGERPRINT_SIZE - 28))
            assert len(fingerprint_records[-1]) == FINGERPRINT_SIZE
        fingerprint_ranges[profile.profile_id] = (
            first_fp,
            len(fingerprint_records) - first_fp,
        )

        first_recipe = len(recipe_records)
        for recipe in profile.recipes:
            # Written by explicit offset, exactly as docs/device-db-format.md
            # section 5.5 tabulates it. Using one long struct format here would
            # make the field order invisible and silently shift every later field
            # if one were ever inserted or dropped; naming each offset removes
            # that whole class of bug.
            record = bytearray(RECIPE_SIZE)
            struct.pack_into("<I", record, 0, profile.profile_id)
            struct.pack_into("<I", record, 4, strings.ref(recipe.domain))
            struct.pack_into("<I", record, 8, strings.ref(recipe.name))
            struct.pack_into("<I", record, 12, strings.ref(recipe.device_class))
            struct.pack_into("<I", record, 16, strings.ref(recipe.unit))
            record[20] = DOMAINS[recipe.domain]
            record[21] = BACKENDS[recipe.backend]
            # 22..23 reserved
            struct.pack_into("<I", record, 24, recipe.read_source_id)
            struct.pack_into("<I", record, 28, recipe.write_target_id)
            struct.pack_into("<I", record, 32, recipe.codec_id)
            struct.pack_into("<I", record, 36, recipe.subscription_id)
            struct.pack_into("<H", record, 40, recipe.endpoint)
            struct.pack_into("<H", record, 42, recipe.cluster)
            struct.pack_into("<H", record, 44, recipe.attribute)
            struct.pack_into("<H", record, 46, recipe.command)
            struct.pack_into("<i", record, 48, recipe.min_value)
            struct.pack_into("<i", record, 52, recipe.max_value)
            struct.pack_into("<I", record, 56, recipe.scale)
            struct.pack_into("<I", record, 60, recipe.flags)
            # 64..71 reserved
            recipe_records.append(bytes(record))
            assert len(recipe_records[-1]) == RECIPE_SIZE
        recipe_ranges[profile.profile_id] = (
            first_recipe,
            len(recipe_records) - first_recipe,
        )

    profile_records: List[bytes] = []
    for index, profile in enumerate(ordered):
        first_fp, fp_count = fingerprint_ranges[profile.profile_id]
        first_recipe, recipe_count = recipe_ranges[profile.profile_id]
        first_identity, identity_count = identity_first[profile.profile_id]

        protocol_mask = 0
        for fp in profile.fingerprints:
            protocol_mask |= 1 << PROTOCOLS[fp.protocol]

        policy = 0
        for name in profile.policy:
            policy |= POLICY[name]

        # Written by explicit offset, exactly as docs/device-db-format.md
        # section 5.3 tabulates it.
        record = bytearray(PROFILE_SIZE)
        struct.pack_into("<I", record, 0, profile.profile_id)
        struct.pack_into("<I", record, 4, strings.ref(profile.vendor))
        struct.pack_into("<I", record, 8, strings.ref(profile.model))
        struct.pack_into("<I", record, 12, strings.ref(profile.display_name))
        struct.pack_into("<I", record, 16, strings.ref(profile.icon))
        # One provenance record per profile, in profile order.
        struct.pack_into("<I", record, 20, index)
        struct.pack_into("<I", record, 24, profile.theengs_decoder_id)
        struct.pack_into("<I", record, 28, profile.zha_quirk_id)
        struct.pack_into("<I", record, 32, first_recipe)
        struct.pack_into("<I", record, 36, recipe_count)
        struct.pack_into("<I", record, 40, policy)
        struct.pack_into("<I", record, 44, first_identity)
        struct.pack_into("<I", record, 48, identity_count)
        record[52] = protocol_mask
        record[53] = 1 if profile.writable else 0
        # 54..55 reserved
        struct.pack_into("<I", record, 56, first_fp)
        struct.pack_into("<I", record, 60, fp_count)
        # 64..95 reserved
        profile_records.append(bytes(record))
        assert len(profile_records[-1]) == PROFILE_SIZE

    # --- index ------------------------------------------------------------
    # Bucket count: next power of two at or above 2x the fingerprint count, so
    # the table stays sparse. Callers may override for fixture purposes.
    total_fp = len(fingerprint_records)
    if index_bucket_count is None:
        bucket_count = 1
        while bucket_count < max(1, total_fp * 2):
            bucket_count <<= 1
    else:
        bucket_count = index_bucket_count
        if bucket_count and (bucket_count & (bucket_count - 1)):
            raise DeviceDbError("index_bucket_count must be a power of two")

    buckets: List[List[Tuple[int, int]]] = [[] for _ in range(bucket_count)]
    overflow: List[Tuple[int, int]] = []
    overflowed = [False] * bucket_count

    # Group by bucket, then assign deterministically.
    by_bucket: Dict[int, List[Tuple[int, int]]] = {}
    for fp_index, record in enumerate(fingerprint_records):
        key_hash = struct.unpack_from("<I", record, 4)[0]
        bucket = key_hash & (bucket_count - 1) if bucket_count else 0
        by_bucket.setdefault(bucket, []).append((key_hash, fp_index))

    for bucket in sorted(by_bucket):
        entries = sorted(by_bucket[bucket])
        inline = entries[:INDEX_ENTRIES_PER_BUCKET]
        extra = entries[INDEX_ENTRIES_PER_BUCKET:]
        buckets[bucket] = inline
        if extra:
            overflowed[bucket] = True
            overflow.extend(extra)

    index_blob = bytearray()
    for bucket in range(bucket_count):
        entries = buckets[bucket]
        cell = bytearray(INDEX_BUCKET_SIZE)
        struct.pack_into("<H", cell, 0, len(entries))
        for i, (key_hash, fp_index) in enumerate(entries):
            struct.pack_into("<II", cell, 2 + i * INDEX_ENTRY_SIZE, key_hash, fp_index)
        struct.pack_into("<H", cell, 30, INDEX_FLAG_OVERFLOW if overflowed[bucket] else 0)
        index_blob.extend(cell)
    for key_hash, fp_index in overflow:
        index_blob.extend(struct.pack("<IIH", key_hash, fp_index, 0))

    # --- assemble sections ------------------------------------------------
    strings_blob = strings.data
    provenance_blob = b"".join(provenance_records)
    identity_blob = b"".join(identity_records)
    profiles_blob = b"".join(profile_records)
    fingerprints_blob = b"".join(fingerprint_records)
    recipes_blob = b"".join(recipe_records)

    sections = [
        strings_blob,
        provenance_blob,
        identity_blob,
        profiles_blob,
        fingerprints_blob,
        recipes_blob,
        bytes(index_blob),
    ]

    offsets = []
    cursor = HEADER_SIZE
    for section in sections:
        offsets.append(cursor)
        cursor += len(section)
        # 8-byte alignment between sections; deterministic zero padding.
        pad = (-cursor) % 8
        cursor += pad

    # Recompute with padding actually inserted.
    cursor = HEADER_SIZE
    padded: List[bytes] = []
    offsets = []
    for section in sections:
        offsets.append(cursor)
        padded.append(section)
        cursor += len(section)
        pad = (-cursor) % 8
        if pad:
            padded.append(b"\x00" * pad)
            cursor += pad
    file_length = cursor

    if file_length > MAX_FILE_BYTES:
        raise DeviceDbError(
            f"generated file is {file_length} bytes, above the "
            f"{MAX_FILE_BYTES} byte cap the reader enforces"
        )

    (strings_off, prov_off, ident_off, prof_off,
     fp_off, recipe_off, index_off) = offsets

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<H", header, 4, FORMAT_VERSION)
    struct.pack_into("<H", header, 6, SCHEMA_VERSION)
    struct.pack_into("<I", header, 8, READER_ABI)
    struct.pack_into("<I", header, 12, 0)  # flags
    struct.pack_into("<I", header, 16, file_length)
    struct.pack_into("<I", header, 20, content_version)
    struct.pack_into("<Q", header, 24, build_timestamp)
    struct.pack_into("<I", header, 32, len(profile_records))
    struct.pack_into("<I", header, 36, len(fingerprint_records))
    struct.pack_into("<I", header, 40, len(recipe_records))
    struct.pack_into("<I", header, 44, len(identity_records))
    struct.pack_into("<I", header, 48, len(provenance_records))
    struct.pack_into("<H", header, 52, bucket_count)
    struct.pack_into("<I", header, 56, strings_off)
    struct.pack_into("<I", header, 60, len(strings_blob))
    struct.pack_into("<I", header, 64, prov_off)
    struct.pack_into("<I", header, 68, len(provenance_blob))
    struct.pack_into("<I", header, 72, ident_off)
    struct.pack_into("<I", header, 76, len(identity_blob))
    struct.pack_into("<I", header, 80, prof_off)
    struct.pack_into("<I", header, 84, len(profiles_blob))
    struct.pack_into("<I", header, 88, fp_off)
    struct.pack_into("<I", header, 92, len(fingerprints_blob))
    struct.pack_into("<I", header, 96, recipe_off)
    struct.pack_into("<I", header, 100, len(recipes_blob))
    struct.pack_into("<I", header, 104, index_off)
    struct.pack_into("<I", header, 108, len(index_blob))

    body = b"".join(padded)
    struct.pack_into("<I", header, 116, crc32_ieee(body))
    struct.pack_into("<I", header, 120, crc32_ieee(bytes(header[0:120])))

    return bytes(header) + body


def digest(image: bytes) -> str:
    """SHA-256 of a generated image, for the reproducibility check."""
    return hashlib.sha256(image).hexdigest()
