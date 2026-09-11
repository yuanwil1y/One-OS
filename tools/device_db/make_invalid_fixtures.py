#!/usr/bin/env python3
"""Derive deliberately damaged Device DB variants from the valid fixture.

Every variant must be REJECTED by the reader. They are generated rather than
hand-committed so each one is provably a single, named mutation of a valid file:
if the base fixture changes, the variants change with it and cannot silently
become valid.

These are test inputs only. Nothing here is compiled into firmware.
"""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from validate_device_db import crc32_ieee  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE_DIR = ROOT / "tests" / "fixtures" / "device_db"
BASE = FIXTURE_DIR / "devices_fixture.nbdb"
OUT_DIR = FIXTURE_DIR / "invalid"


def _refresh_crc(body_owner: bytearray) -> None:
    """Recompute both CRCs so a mutation is not caught merely by the checksum.

    Each structural variant must be rejected because of the rule it breaks, not
    because the CRC happens to notice. Recomputation is what makes that true.
    """
    file_length = len(body_owner)
    struct.pack_into("<I", body_owner, 16, file_length)
    body_crc = crc32_ieee(bytes(body_owner[128:]))
    struct.pack_into("<I", body_owner, 116, body_crc)
    header_crc = crc32_ieee(bytes(body_owner[0:120]))
    struct.pack_into("<I", body_owner, 120, header_crc)


def _region(data: bytes, name: str) -> tuple[int, int]:
    offsets = {
        "strings": 56,
        "provenance": 64,
        "identity": 72,
        "profiles": 80,
        "fingerprints": 88,
        "recipes": 96,
    }
    length_offsets = {
        "strings": 60,
        "provenance": 68,
        "identity": 76,
        "profiles": 84,
        "fingerprints": 92,
        "recipes": 100,
    }
    return (struct.unpack_from("<I", data, offsets[name])[0],
            struct.unpack_from("<I", data, length_offsets[name])[0])


def variants(base: bytes) -> dict[str, bytes]:
    out: dict[str, bytes] = {}

    # --- structural, caught before any record is read ---------------------
    out["bad_magic"] = b"XXXX" + base[4:]

    v = bytearray(base)
    struct.pack_into("<H", v, 4, 99)
    _refresh_crc(v)
    out["unknown_format_version"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<H", v, 6, 99)
    _refresh_crc(v)
    out["unknown_schema_version"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<I", v, 8, 99)
    _refresh_crc(v)
    out["reader_abi_too_new"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<I", v, 12, 1)
    _refresh_crc(v)
    out["reserved_flags_set"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<I", v, 16, len(base) + 8)
    out["file_length_lies"] = bytes(v)

    out["truncated_header"] = base[:100]

    out["truncated_body"] = base[: len(base) - 64]

    v = bytearray(base)
    struct.pack_into("<I", v, 116, 0xDEADBEEF)
    out["body_crc_mismatch"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<I", v, 120, 0xDEADBEEF)
    out["header_crc_mismatch"] = bytes(v)

    # --- region bounds ----------------------------------------------------
    v = bytearray(base)
    struct.pack_into("<I", v, 80, len(base) + 4096)  # profiles outside the file
    _refresh_crc(v)
    out["profiles_region_outside_file"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<I", v, 84, struct.unpack_from("<I", v, 84)[0] + 8)
    _refresh_crc(v)
    out["profiles_length_not_count_times_size"] = bytes(v)

    # profile_count larger than the region can hold (integer-overflow probe)
    v = bytearray(base)
    struct.pack_into("<I", v, 32, 0xFFFFFFFF)
    _refresh_crc(v)
    out["profile_count_overflow"] = bytes(v)

    v = bytearray(base)
    struct.pack_into("<H", v, 52, 6)  # not a power of two
    _refresh_crc(v)
    out["index_bucket_count_not_power_of_two"] = bytes(v)

    # --- record-level rules ----------------------------------------------
    prof_off, _ = _region(base, "profiles")
    recipes_off, _ = _region(base, "recipes")
    fps_off, _ = _region(base, "fingerprints")
    index_off = struct.unpack_from("<I", base, 104)[0]

    # Duplicate profile id: copy profile 0's id into profile 1.
    v = bytearray(base)
    first_id = struct.unpack_from("<I", base, prof_off)[0]
    struct.pack_into("<I", v, prof_off + 96, first_id)
    _refresh_crc(v)
    out["duplicate_profile_id"] = bytes(v)

    # Zero profile id.
    v = bytearray(base)
    struct.pack_into("<I", v, prof_off, 0)
    _refresh_crc(v)
    out["zero_profile_id"] = bytes(v)

    # provenance_index out of range.
    v = bytearray(base)
    struct.pack_into("<I", v, prof_off + 20, 0xFFFF)
    _refresh_crc(v)
    out["provenance_index_out_of_range"] = bytes(v)

    # fingerprint_count = 0 means the profile could never match.
    v = bytearray(base)
    struct.pack_into("<I", v, prof_off + 60, 0)
    _refresh_crc(v)
    out["profile_without_fingerprint"] = bytes(v)

    # writable without a writable recipe: take the BLE profile (read-only
    # recipes only) and mark it writable.
    v = bytearray(base)
    v[prof_off + 53] = 1
    _refresh_crc(v)
    out["writable_without_writable_recipe"] = bytes(v)

    # recipe referencing an unknown profile.
    v = bytearray(base)
    struct.pack_into("<I", v, recipes_off, 0x7FFFFFFF)
    _refresh_crc(v)
    out["recipe_unknown_profile"] = bytes(v)

    # fingerprint referencing an unknown profile.
    v = bytearray(base)
    struct.pack_into("<I", v, fps_off + 12, 0x7FFFFFFF)
    _refresh_crc(v)
    out["fingerprint_unknown_profile"] = bytes(v)

    # fingerprint key_hash that cannot match its own key.
    v = bytearray(base)
    struct.pack_into("<I", v, fps_off + 4, 0x12345678)
    _refresh_crc(v)
    out["fingerprint_key_hash_mismatch"] = bytes(v)

    # min_value > max_value on the first recipe.
    v = bytearray(base)
    struct.pack_into("<i", v, recipes_off + 48, 1000)
    struct.pack_into("<i", v, recipes_off + 52, -1000)
    _refresh_crc(v)
    out["recipe_min_above_max"] = bytes(v)

    # zigbee recipe missing its endpoint.
    # Find the zigbee command recipe: profile 1002 is the second profile.
    zig_recipe = None
    for i in range(struct.unpack_from("<I", base, 40)[0]):
        base_i = recipes_off + i * 72
        if base[base_i + 21] == 5:  # ZIGBEE_COMMAND
            zig_recipe = base_i
            break
    if zig_recipe is not None:
        v = bytearray(base)
        struct.pack_into("<H", v, zig_recipe + 40, 0xFFFF)
        _refresh_crc(v)
        out["zigbee_recipe_missing_endpoint"] = bytes(v)

    # a 'none' backend recipe that claims to be writable.
    none_recipe = None
    for i in range(struct.unpack_from("<I", base, 40)[0]):
        base_i = recipes_off + i * 72
        if base[base_i + 21] == 0:  # BACKEND_NONE
            none_recipe = base_i
            break
    if none_recipe is not None:
        v = bytearray(base)
        struct.pack_into("<I", v, none_recipe + 28, 0)
        _refresh_crc(v)
        out["none_backend_is_writable"] = bytes(v)

    # write_control without user_action_required.
    v = bytearray(base)
    policy = struct.unpack_from("<I", base, prof_off + 40)[0]
    struct.pack_into("<I", v, prof_off + 40, policy | (1 << 4))  # WRITE_CONTROL
    _refresh_crc(v)
    out["write_control_without_user_action"] = bytes(v)

    # index entry pointing past the fingerprint region.
    #
    # A bucket is: entry_count(u16 at 0), then entries of
    # key_hash(u32) + fingerprint_index(u32). So the fp_index of entry e lives at
    # 2 + e*10 + 4. Mutating the key_hash instead would not be an error at all,
    # which is exactly the trap this variant has to avoid.
    index_buckets = struct.unpack_from("<H", base, 52)[0]
    target = None
    for b in range(index_buckets):
        b_off = index_off + b * 32
        if struct.unpack_from("<H", base, b_off)[0] > 0:
            target = b_off
            break
    if target is not None:
        v = bytearray(base)
        struct.pack_into("<I", v, target + 2 + 4, 0x00FFFFFF)
        _refresh_crc(v)
        out["index_entry_out_of_range"] = bytes(v)

    # bucket declares more entries than it can hold.
    v = bytearray(base)
    struct.pack_into("<H", v, index_off, 9)
    _refresh_crc(v)
    out["index_bucket_entry_count_invalid"] = bytes(v)

    # unsafe identity kind declared with a usable strength.
    ident_off, ident_len = _region(base, "identity")
    if ident_len >= 24:
        v = bytearray(base)
        v[ident_off] = 7  # BLE_RANDOM_ADDRESS: never safe
        v[ident_off + 1] = 2  # STRONG
        _refresh_crc(v)
        out["unsafe_identity_declared_strong"] = bytes(v)

    return out


def main() -> int:
    if not BASE.exists():
        print(f"missing base fixture {BASE}; run build_device_db.py first",
              file=sys.stderr)
        return 1
    base = BASE.read_bytes()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    written = variants(base)
    for name, blob in sorted(written.items()):
        (OUT_DIR / f"{name}.nbdb").write_bytes(blob)

    # A manifest so the C test knows exactly which files must be rejected and
    # does not silently skip a missing one.
    lines = [f"{name}.nbdb" for name in sorted(written)]
    (OUT_DIR / "manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"wrote {len(written)} invalid variants to "
          f"{OUT_DIR.relative_to(ROOT)}")
    for name in sorted(written):
        print(f"  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
