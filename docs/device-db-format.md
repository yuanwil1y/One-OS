# One-OS Device DB (`.nbdb`) format specification

Status: **normative format definition for task B4**
File name on target: `/nearby/db/devices.nbdb`
Primary implementation: `firmware/main/device_db_format.c` (reader/validator, shared by
firmware and host tools)

This document defines the binary container. The generator/validator live in
`tools/device_db/`. Nothing here is generated automatically into the firmware:
the production corpus lives only on SD, and the only `.nbdb` files in this
repository are test fixtures.

---

## 1. Relationship to the previous NearBy One NEXT `.nbdb`

**The previous project's source is not available in this environment.** Only its
reuse matrix survives, in
[`docs/application/provisioning-web-management.md`](../application/provisioning-web-management.md)
section 9, which describes `firmware/components/db_storage/db_storage.c` and a
browser-side `.nbdb` preflight check but not the file layout.

Consequently this specification **does not assume wire compatibility with the old
`.nbdb`**. The extension is reused because the product documents fix the file name
`devices.nbdb`; the *container* is defined from scratch and carries explicit
version numbers so that:

- a file this reader does not understand is rejected as `INCOMPATIBLE` rather than
  misread;
- if the old format is later recovered, a one-way converter can be written against
  `format_version = 1` without touching the reader.

What is intentionally reused from the provisioning guide:

- the staged-replacement workflow (`.part` → validate → promote);
- firmware-side validation being authoritative even if the browser pre-validates;
- the requirement that a failed upload leaves the previous DB valid.

What is intentionally **not** reused: the old "format the whole SD card" flow and
the `/api/db/format` endpoint. Both are excluded by the product rules.

---

## 2. Conventions

| Property | Value |
|---|---|
| Byte order | **little-endian**, every integer field |
| Alignment | all sections start at an 8-byte boundary |
| Checksum | CRC-32 (IEEE 802.3, reflected, init `0xFFFFFFFF`, final xor `0xFFFFFFFF`) |
| Strings | byte strings, **not** NUL-terminated in the file; referenced as `(offset,length)` |
| Text encoding | UTF-8, but the firmware treats it as opaque bytes and copies it bounded |
| Padding | zero-filled; readers must not depend on padding content |
| Maximum file size | 16 MiB (enforced; prevents hostile `size` fields) |

Every multi-byte field is written with explicit byte assembly, never by
`memcpy`-ing a struct. Struct layout is therefore not part of the format and a
compiler cannot silently change it.

---

## 3. File layout

```text
+---------------------------+
| Header (128 bytes)        |
+---------------------------+
| Provenance records        |  provenance_count * 24
+---------------------------+
| Identity rules            |  identity_rule_count * 24
+---------------------------+
| Profiles                  |  profile_count * 96
+---------------------------+
| Strings                   |  string_bytes
+---------------------------+
| Fingerprints              |  fingerprint_count * 40
+---------------------------+
| Entity recipes            |  recipe_count * 72
+---------------------------+
| Fingerprint index buckets |  index_bucket_count * 32
+---------------------------+
```

All eight regions are described by an offset **and** a length in the header.
A reader must reject the file if any region, or any record inside it, extends
beyond `file_length`.

---

## 4. Header

Exactly 128 bytes. Every field is written little-endian.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | `"NBDB"` = `0x4E 0x42 0x44 0x42` |
| 4 | 2 | `format_version` | container layout. This document defines **1** |
| 6 | 2 | `schema_version` | meaning of record contents. This document defines **1** |
| 8 | 4 | `reader_abi` | minimum reader capability required |
| 12 | 4 | `flags` | reserved, must be `0` |
| 16 | 4 | `file_length` | total bytes, must equal the actual file size |
| 20 | 4 | `content_version` | generator's corpus revision, e.g. `20260911` |
| 24 | 8 | `build_timestamp` | Unix seconds; `0` means "reproducible build" |
| 32 | 4 | `profile_count` | |
| 36 | 4 | `fingerprint_count` | |
| 40 | 4 | `recipe_count` | |
| 44 | 4 | `identity_rule_count` | |
| 48 | 4 | `provenance_count` | |
| 52 | 2 | `index_bucket_count` | must be `0` or a power of two |
| 54 | 2 | `reserved0` | 0 |
| 56 | 4 | `strings_offset` | |
| 60 | 4 | `strings_length` | |
| 64 | 4 | `provenance_offset` | |
| 68 | 4 | `provenance_length` | |
| 72 | 4 | `identity_offset` | |
| 76 | 4 | `identity_length` | |
| 80 | 4 | `profiles_offset` | |
| 84 | 4 | `profiles_length` | |
| 88 | 4 | `fingerprints_offset` | |
| 92 | 4 | `fingerprints_length` | |
| 96 | 4 | `recipes_offset` | |
| 100 | 4 | `recipes_length` | |
| 104 | 4 | `index_offset` | |
| 108 | 4 | `index_length` | |
| 112 | 4 | `reserved1` | 0 |
| 116 | 4 | `body_crc32` | CRC of `[128, file_length)` |
| 120 | 4 | `header_crc32` | CRC of `[0, 120)` |
| 124 | 4 | `reserved2` | 0 |

The header is exactly 128 bytes; the first section starts at offset 128. Offsets
are 32-bit, which combined with the 16 MiB size cap keeps all comparisons inside
`uint32_t` while still allowing a comfortable future corpus.

### 4.1 Checksum coverage

Two CRCs exist and they cover **different** things, deliberately:

| Field | Covers |
|---|---|
| `body_crc32` (offset 116) | all bytes **after** the header: `[128, file_length)` |
| `header_crc32` (offset 120) | header bytes `[0, 120)`, i.e. everything before the CRC fields |

Splitting them lets a reader report "header corrupt" separately from "body
corrupt", and means a damaged body cannot be hidden by recomputing one checksum
over the whole file.

### 4.2 `reader_abi`

The ABI number is a capability gate, not a layout number. A reader refuses a file
whose `reader_abi` exceeds the ABI it implements, even when `format_version` is
recognised. This is what allows a future generator to emit records a current
reader would silently misinterpret.

Defined values:

| ABI | Adds |
|---:|---|
| 1 | profiles, identity rules, fingerprints, entity recipes, provenance |

---

## 5. Records

All records are fixed-size and little-endian. Unused tail bytes are zero.

### 5.1 Provenance record (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `source_id` — index into the generator's source manifest |
| 4 | 1 | `reuse` — `0 COPY`, `1 PORT`, `2 CLEAN_ROOM`, `3 REFERENCE_ONLY` |
| 5 | 1 | `license` — enum, see below |
| 6 | 2 | `reserved` (0) |
| 8 | 4 | `source_revision` — generator-defined revision token |
| 12 | 4 | `source_string` — `StringRef` to the upstream project name |
| 16 | 4 | `review_string` — `StringRef` to a human-readable justification |
| 20 | 4 | `reserved` (0) |

`license` enum: `0 UNKNOWN`, `1 MIT`, `2 BSD_2`, `3 BSD_3`, `4 APACHE_2`,
`5 CC0`, `6 PUBLIC_DOMAIN`, `7 PROPRIETARY_CLEAN_ROOM`.

A record with `reuse = REFERENCE_ONLY` may not, on its own, justify a profile.
The generator enforces this; see section 7.

### 5.2 Identity rule (24 bytes)

Identity rules are how the application decides two observations are the same
physical device. They are data, not code: a rule names a *published protocol
identifier*, it never contains an expression.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `kind` — see below |
| 1 | 1 | `strength` — `0 UNSAFE`, `1 WEAK`, `2 STRONG` |
| 2 | 2 | `reserved` |
| 4 | 4 | `key_string` — `StringRef`, rule parameter (e.g. domain name) |
| 8 | 4 | `profile_id` |
| 12 | 4 | `flags` — bit 0 `LOCAL_ONLY`, bit 1 `REQUIRES_AUTHORIZATION` |
| 16 | 8 | `reserved` |
`kind`:

| Value | Name | Safe cross-protocol merge? |
|---:|---|---|
| 0 | `NONE` | no |
| 1 | `ZIGBEE_IEEE` | yes (EUI-64) |
| 2 | `MATTER_NODE_FABRIC` | yes, scoped to the local fabric |
| 3 | `VENDOR_PROTOCOL_UUID` | yes, when the profile asserts it |
| 4 | `ESPHOME_NODE_NAME` | yes |
| 5 | `MDNS_TXT_IDENTIFIER` | only when documentation guarantees meaning |
| 6 | `BLE_PUBLIC_ADDRESS` | only when the profile says the device is static |
| 7 | `BLE_RANDOM_ADDRESS` | **never** — `strength` must be `UNSAFE` |
| 8 | `WIFI_BSSID` | **never** as physical identity |
| 9 | `IP_ADDRESS` | **never** |
| 10 | `RSSI` | **never** |
| 11 | `SSID` | **never** |
| 12 | `MODEL_NAME` | **never** |
| 13 | `MATTER_VID_PID` | **never** on its own |

The validator rejects a rule whose `kind` is in the "never" set unless
`strength = UNSAFE`, so an unsafe rule can be recorded for diagnostics without
ever being usable for merging.

### 5.3 Profile record (96 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `profile_id` (non-zero, unique, ascending in the file) |
| 4 | 4 | `vendor_string` — `StringRef` |
| 8 | 4 | `model_string` — `StringRef` |
| 12 | 4 | `display_name_string` — `StringRef` |
| 16 | 4 | `icon_string` — `StringRef` |
| 20 | 4 | `provenance_index` |
| 24 | 4 | `theengs_decoder_id` — `0xFFFFFFFF` = none |
| 28 | 4 | `zha_quirk_id` — `0xFFFFFFFF` = none |
| 32 | 4 | `first_recipe_index` |
| 36 | 4 | `recipe_count` |
| 40 | 4 | `policy_flags` — allowed automatic operations, see 5.6 |
| 44 | 4 | `identity_rule_first` |
| 48 | 4 | `identity_rule_count` |
| 52 | 1 | `protocol_mask` — which protocols may match this profile |
| 53 | 1 | `writable` — `0` unless a verified control path exists |
| 54 | 2 | `reserved` |
| 56 | 4 | `fingerprint_first` |
| 60 | 4 | `fingerprint_count` |
| 64 | 4 | `reserved[8]` |

`protocol_mask` bits: 0 BLE, 1 Wi-Fi, 2 mDNS, 3 SSDP, 4 LAN service, 5 Zigbee,
6 Matter, 7 ESPHome.

A profile with `writable = 1` must have at least one recipe whose backend is not
`NONE` or `PASSIVE_VALUE`; the validator rejects the file otherwise. This encodes
"a database match may only suggest control when a real backend exists".

### 5.4 Fingerprint record (40 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `protocol` — same bit numbering as `protocol_mask`, but a single value |
| 1 | 1 | `match_kind` — `0 EXACT`, `1 PREFIX`, `2 BITMASK` |
| 2 | 2 | `reserved` |
| 4 | 4 | `key_hash` — FNV-1a 32 of the canonical key bytes |
| 8 | 4 | `key_string` — `StringRef` to the canonical key |
| 12 | 4 | `profile_id` |
| 16 | 4 | `mask_length` — for `BITMASK` |
| 20 | 8 | `mask_string` — `StringRef` to the mask bytes |
| 28 | 4 | `flags` — bit 0 `TRUNCATED_KEY`, bit 1 `AMBIGUITY_MARKER` |
| 32 | 8 | `reserved` |

The canonical key is **not** free text. It is the profile's declared fingerprint
in a normalised form defined per protocol by the generator (`device_db` keys are
lowercase hex without separators for addresses, and the exact
service/manufacturer bytes as hex for BLE payloads). The firmware compares
`key_hash` first and only then the key bytes, so a hash collision cannot cause a
false match.

### 5.4.1 Key normalisation is part of the contract

`key_string` stores the **human-readable declaration** (for example
`Example|Plug-ZB-2` or `_ipp._tcp.local`), but `key_hash` covers the **canonical
form**:

```text
canonical(key) = lowercase( trim(key) ) with all ':' and '-' removed
```

So `AA:BB:CC:DD:EE:FF`, `aa-bb-cc-dd-ee-ff` and `aabbccddeeff` all hash
identically. This rule must be applied identically by the generator, the firmware
reader and the host validator; if any of the three hashes the raw bytes instead,
a valid file is reported as corrupt.

Both spellings are therefore stored on purpose:

- `key_string` stays readable, so a human reading a hex dump or a diagnostics
  screen can tell what the fingerprint is;
- `key_hash` stays comparable, so matching is a single integer compare.

The reader re-derives `key_hash` from `key_string` during validation and rejects
the file if they disagree, which is what makes the pair self-checking.

`DEVICE_DB_CANONICAL_KEY_HASH` in `device_db_format.h` is the reference
implementation of this rule.

### 5.5 Entity recipe record (72 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `profile_id` |
| 4 | 4 | `domain_string` — `StringRef`, HA domain (`sensor`, `switch`, ...) |
| 8 | 4 | `name_string` — `StringRef` |
| 12 | 4 | `device_class_string` — `StringRef` |
| 16 | 4 | `unit_string` — `StringRef` |
| 20 | 1 | `domain_id` — numeric HA domain enum |
| 21 | 1 | `backend` — see 5.6 |
| 22 | 2 | `reserved` (0) |
| 24 | 4 | `read_source_id` — decoder property / cluster-attribute id |
| 28 | 4 | `write_target_id` — `0xFFFFFFFF` = read-only |
| 32 | 4 | `codec_id` — `0xFFFFFFFF` = no codec |
| 36 | 4 | `subscription_id` — `0xFFFFFFFF` = none |
| 40 | 2 | `endpoint` — Zigbee/Matter endpoint, `0xFFFF` = n/a |
| 42 | 2 | `cluster` — `0xFFFF` = n/a |
| 44 | 2 | `attribute` — `0xFFFF` = n/a |
| 46 | 2 | `command` — `0xFFFF` = n/a |
| 48 | 4 | `min_value` — as `int32`, scaled |
| 52 | 4 | `max_value` — as `int32`, scaled |
| 56 | 4 | `scale` — decimal places |
| 60 | 4 | `flags` |
| 64 | 8 | `reserved` (0) |

Fields occupy 64 bytes; 8 bytes of trailing zero padding complete the 72-byte
record.

`domain_id`: `0 SENSOR`, `1 BINARY_SENSOR`, `2 SWITCH`, `3 LIGHT`, `4 BUTTON`,
`5 NUMBER`, `6 SELECT`, `7 CLIMATE`.

`backend`: `0 NONE`, `1 PASSIVE_VALUE`, `2 BLE_GATT`, `3 ESPHOME_API`,
`4 ZIGBEE_ATTRIBUTE`, `5 ZIGBEE_COMMAND`, `6 MATTER_ATTRIBUTE`,
`7 MATTER_COMMAND`.

Cross-field rules the validator enforces:

- `ZIGBEE_*` requires `endpoint`, `cluster` to be set;
- `ZIGBEE_COMMAND` and `MATTER_COMMAND` require `command` to be set;
- `ZIGBEE_ATTRIBUTE` and `MATTER_ATTRIBUTE` require `attribute` to be set;
- `MATTER_*` requires `endpoint`;
- `PASSIVE_VALUE` requires `read_source_id`;
- `NONE` may not set `write_target_id`;
- `min_value <= max_value` when both are non-sentinel.

### 5.6 Policy flags

`policy_flags` on a profile (recorded so a future scan can decide what it may do
automatically, per the product rules):

| Bit | Name |
|---:|---|
| 0 | `PASSIVE_ONLY` |
| 1 | `SAFE_READ` |
| 2 | `AUTH_REQUIRED` |
| 3 | `USER_ACTION_REQUIRED` |
| 4 | `WRITE_CONTROL` |

`WRITE_CONTROL` without `USER_ACTION_REQUIRED` is rejected: a state-changing
operation always requires explicit user action.

---

## 6. Fingerprint index

The index is a hash table with **power-of-two bucket count**, so a bucket is
found by masking rather than by division:

```text
bucket = key_hash & (index_bucket_count - 1)
```

Each bucket is 32 bytes and holds up to 3 entries of 10 bytes plus a 2-byte
count:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | `entry_count` (0..3) |
| 2 | 30 | `entries[3]` |

Each entry:

| Size | Field |
|---:|---|
| 4 | `key_hash` |
| 4 | `fingerprint_index` |
| 2 | `flags` — bit 0 `BUCKET_OVERFLOW` |

**Overflow semantics.** The builder never drops an entry. When a bucket already
holds 3 entries the extra entries are placed in a contiguous overflow run that
immediately follows the bucket array, and `BUCKET_OVERFLOW` is set on the bucket
so a reader knows the bucket's inline entries are not exhaustive. A reader that
sees the flag must not conclude "not found"; it must fall back to scanning
candidate fingerprints for that protocol. Correctness never depends on the index:
the index is an accelerator, and every match is confirmed against the actual
fingerprint record.

---

## 7. Provenance gate

The generator refuses to emit a profile whose every contributing provenance
record is `REFERENCE_ONLY`. Such records may be listed for documentation but
cannot justify shipping recognition data. This is the machine-checkable half of
the product rule "records sourced only from `REFERENCE_ONLY` material require an
independent specification, owned capture or clean-room derivation before
shipping".

The generator also refuses:

- a profile with no provenance record;
- a provenance record whose `source_id` is absent from the source manifest;
- a provenance record whose `reuse` is `REFERENCE_ONLY` while the manifest marks
  the source as `permitted_for_output`.

---

## 8. Rejection rules (what a reader must refuse)

A reader must classify a file rather than trust it. Minimum set:

| Condition | Result |
|---|---|
| size < 128 | `TRUNCATED` |
| bad magic | `NOT_A_DB` |
| `format_version` unknown | `INCOMPATIBLE` |
| `reader_abi` > implemented | `INCOMPATIBLE` |
| `flags != 0` | `INCOMPATIBLE` |
| `file_length != actual size` | `CORRUPT` |
| `header_crc32` mismatch | `CORRUPT` |
| `body_crc32` mismatch | `CORRUPT` |
| any region outside `[128, file_length]` | `CORRUPT` |
| any region end < region start (overflow) | `CORRUPT` |
| `index_bucket_count` not a power of two | `CORRUPT` |
| `profile_id == 0` or duplicate | `CORRUPT` |
| string ref outside the strings region | `CORRUPT` |
| string ref with `length == 0` where a name is required | `CORRUPT` |
| `provenance_index` out of range | `CORRUPT` |
| `*_first + *_count` exceeds the region | `CORRUPT` |
| recipe `profile_id` unknown | `CORRUPT` |
| identity rule `profile_id` unknown | `CORRUPT` |
| fingerprint `profile_id` unknown | `CORRUPT` |
| index entry `fingerprint_index` out of range | `CORRUPT` |
| `writable = 1` with no writable recipe | `CORRUPT` |
| unsafe identity rule with `strength != UNSAFE` | `CORRUPT` |
| `WRITE_CONTROL` without `USER_ACTION_REQUIRED` | `CORRUPT` |
| `min_value > max_value` | `CORRUPT` |
| profile with no fingerprint | `CORRUPT` |

All arithmetic is performed in 64-bit and range-checked before use, so an
overflowing `offset + length` is detected rather than wrapping.

---

## 9. Determinism

The generator must be reproducible: the same source manifest and profile input
must produce byte-identical output. Therefore:

- profiles are emitted sorted by `profile_id`;
- fingerprints are emitted sorted by `(protocol, key_hash, profile_id)`;
- recipes are emitted grouped by profile, then by declaration order;
- strings are emitted in first-use order with deduplication;
- `build_timestamp` is `0` unless explicitly overridden;
- index bucket contents are sorted by `(key_hash, fingerprint_index)`.

`tools/device_db/build_device_db.py` is checked in with a fixture input and an
expected SHA-256, so a non-deterministic change fails a test rather than silently
altering the shipped corpus.

---

## 10. Fixtures and production corpus

| Artefact | Purpose | Ships in firmware? |
|---|---|---|
| `tools/device_db/sources/*.json` | declaration of upstream sources + provenance | no |
| `tools/device_db/profiles/*.json` | canonical profile input | no |
| `tests/fixtures/device_db/*.nbdb` | test corpus (known/unknown/ambiguous/no-merge) | **no** |
| `/nearby/db/devices.nbdb` on SD | the only production recognition corpus | on SD, never in flash |

No fixture is compiled into firmware, and there is no fallback matcher in flash.
When the SD database is missing or unreadable the application keeps generic
unknown Devices and skips recognition.
