#!/usr/bin/env python3
"""Host tests for the Device DB generator and validator.

The C reader has its own test group. This one covers the generator and validator
rules that the C side cannot: determinism, the provenance gate, and the specific
cross-field rules the generator must refuse before it ever writes a file.

Every rejection case is written as "this input must NOT produce a file", so a
relaxed rule fails here instead of shipping a permissive database.
"""

from __future__ import annotations

import io
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import nbdb  # noqa: E402
import validate_device_db as validator  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE_DIR = ROOT / "tests" / "fixtures" / "device_db"

failures = 0
checks = 0


def check(cond: bool, what: str) -> None:
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}")


def expect_refused(what: str, fn) -> None:
    """The generator must raise DeviceDbError for this input."""
    global failures, checks
    checks += 1
    try:
        fn()
    except nbdb.DeviceDbError:
        return
    except Exception as exc:  # noqa: BLE001 - a different error is still a failure
        failures += 1
        print(f"FAIL: {what}: raised {type(exc).__name__} instead of DeviceDbError")
        return
    failures += 1
    print(f"FAIL: {what}: generator accepted input it must refuse")


# --- helpers --------------------------------------------------------------


def base_provenance(source_id: int = 1, reuse: str = "copy") -> nbdb.Provenance:
    return nbdb.Provenance(
        source_id=source_id,
        reuse=reuse,
        license="public_domain",
        source_revision="20260911",
        source_name="one-os-owned-fixtures",
        review="test",
    )


def simple_profile(profile_id: int = 1, **overrides) -> nbdb.Profile:
    defaults = dict(
        profile_id=profile_id,
        vendor="V",
        model="M",
        display_name="D",
        icon="",
        provenance=base_provenance(),
        fingerprints=[nbdb.Fingerprint(protocol="ble", key="aabb")],
        recipes=[],
        identity_rules=[],
        policy=["passive_only"],
        writable=False,
    )
    defaults.update(overrides)
    return nbdb.Profile(**defaults)


def build_simple(profiles, **kwargs) -> bytes:
    params = dict(
        content_version=1,
        known_source_ids=[1, 2, 3, 4, 5],
        reference_only_sources=[2, 3, 4],
    )
    params.update(kwargs)
    return nbdb.build(profiles, **params)


# --- tests ----------------------------------------------------------------


def test_crc_and_hash_match_reference_values() -> None:
    check(nbdb.crc32_ieee(b"123456789") == 0xCBF43926, "CRC-32 check value")
    check(nbdb.fnv1a32(b"") == 2166136261, "FNV-1a offset basis")
    check(nbdb.fnv1a32(b"a") == 0xE40C292C, "FNV-1a of 'a'")


def test_determinism() -> None:
    profiles = [simple_profile(2), simple_profile(1), simple_profile(3)]
    a = build_simple(profiles)
    b = build_simple(list(reversed(profiles)))
    check(a == b, "output must not depend on input ordering (ids are sorted)")

    # Rebuilding from the same objects must also be identical.
    check(build_simple(profiles) == a, "rebuild must be byte-identical")


def test_provenance_gate_rejects_reference_only() -> None:
    # reuse class says reference_only
    expect_refused(
        "profile citing only a reference_only reuse class",
        lambda: build_simple([simple_profile(provenance=base_provenance(1, "reference_only"))],
                             reference_only_sources=[]),
    )
    # the source manifest has not permitted output for this source
    expect_refused(
        "profile citing a source whose output is not permitted",
        lambda: build_simple([simple_profile(provenance=base_provenance(2, "port"))]),
    )
    # unknown source id
    expect_refused(
        "profile citing a source absent from the manifest",
        lambda: build_simple([simple_profile(provenance=base_provenance(99))]),
    )
    # no fingerprints at all
    expect_refused(
        "profile with no fingerprint",
        lambda: build_simple([simple_profile(fingerprints=[])]),
    )
    # duplicate ids
    expect_refused(
        "duplicate profile_id",
        lambda: build_simple([simple_profile(5), simple_profile(5)]),
    )
    expect_refused(
        "profile_id 0",
        lambda: build_simple([simple_profile(0)]),
    )


def test_recipe_rules_are_enforced() -> None:
    expect_refused(
        "none backend marked writable",
        lambda: build_simple([simple_profile(
            recipes=[nbdb.Recipe(domain="switch", backend="none", write_target_id=0)])]),
    )
    expect_refused(
        "passive_value marked writable",
        lambda: build_simple([simple_profile(
            recipes=[nbdb.Recipe(domain="sensor", backend="passive_value",
                                 write_target_id=0)])]),
    )
    expect_refused(
        "zigbee recipe without endpoint",
        lambda: build_simple([simple_profile(
            recipes=[nbdb.Recipe(domain="switch", backend="zigbee_command",
                                 cluster=6, command=1)])]),
    )
    expect_refused(
        "zigbee command recipe without a command id",
        lambda: build_simple([simple_profile(
            recipes=[nbdb.Recipe(domain="switch", backend="zigbee_command",
                                 endpoint=1, cluster=6)])]),
    )
    expect_refused(
        "min_value above max_value",
        lambda: build_simple([simple_profile(
            recipes=[nbdb.Recipe(domain="sensor", backend="none",
                                 min_value=10, max_value=1)])]),
    )
    expect_refused(
        "writable profile with no writable recipe",
        lambda: build_simple([simple_profile(
            writable=True,
            recipes=[nbdb.Recipe(domain="sensor", backend="passive_value")])]),
    )

    # A valid writable profile must still be accepted, so the rules above are not
    # simply rejecting everything.
    image = build_simple([simple_profile(
        writable=True,
        policy=["safe_read", "user_action_required", "write_control"],
        recipes=[nbdb.Recipe(domain="switch", backend="zigbee_command",
                             endpoint=1, cluster=6, command=1, write_target_id=0)])])
    check(len(image) > 128, "a valid writable profile must build")


def test_policy_and_identity_rules() -> None:
    expect_refused(
        "write_control without user_action_required",
        lambda: build_simple([simple_profile(policy=["write_control"])]),
    )
    expect_refused(
        "unknown policy flag",
        lambda: build_simple([simple_profile(policy=["not_a_flag"])]),
    )
    expect_refused(
        "unsafe identity kind declared strong",
        lambda: build_simple([simple_profile(identity_rules=[
            nbdb.IdentityRule(kind="ip_address", strength="strong")])]),
    )
    expect_refused(
        "identity kind 'none' declared strong",
        lambda: build_simple([simple_profile(identity_rules=[
            nbdb.IdentityRule(kind="none", strength="strong")])]),
    )
    # The safe variant of the same kind must be accepted.
    image = build_simple([simple_profile(identity_rules=[
        nbdb.IdentityRule(kind="zigbee_ieee", strength="strong")])])
    check(len(image) > 128, "a safe identity rule must build")


def test_string_table_limits() -> None:
    expect_refused(
        "string longer than 255 bytes",
        lambda: build_simple([simple_profile(vendor="x" * 300)]),
    )


def test_validator_accepts_committed_fixture() -> None:
    path = FIXTURE_DIR / "devices_fixture.nbdb"
    check(path.exists(), f"fixture exists at {path}")
    if not path.exists():
        return
    db = validator.validate(path.read_bytes())
    # 1006 is the ESPHome profile: an mDNS-instance-keyed node with an ESPHOME_API recipe.
    check(db.info.profile_count == 6, "fixture has 6 profiles")
    check(db.info.build_timestamp == 0, "fixture timestamp is 0 (reproducible)")

    # The ambiguous pair really shares a key.
    keys = {}
    for fp in db.fingerprints():
        keys.setdefault((fp["protocol"], fp["key_hash"]), []).append(fp["profile_id"])
    shared = [v for v in keys.values() if len(v) > 1]
    check(len(shared) >= 1, "fixture must contain at least one ambiguous key")

    # No provenance in the fixture may be reference-only.
    for prov in db.provenances():
        check(prov["reuse"] != validator.REUSE_REFERENCE_ONLY
              if hasattr(validator, "REUSE_REFERENCE_ONLY") else prov["reuse"] != 3,
              "fixture provenance must not be reference-only")


def test_validator_rejects_every_variant() -> None:
    manifest = FIXTURE_DIR / "invalid" / "manifest.txt"
    check(manifest.exists(), "invalid-variant manifest exists")
    if not manifest.exists():
        return
    names = [n.strip() for n in manifest.read_text().splitlines() if n.strip()]
    check(len(names) >= 25, f"expected >=25 invalid variants, found {len(names)}")
    for name in names:
        blob = (FIXTURE_DIR / "invalid" / name).read_bytes()
        try:
            validator.validate(blob)
        except validator.Invalid:
            continue
        except Exception as exc:  # noqa: BLE001
            check(False, f"{name}: raised {type(exc).__name__} instead of Invalid")
            continue
        check(False, f"{name}: validator ACCEPTED a damaged file")


def test_validator_rejects_truncations_of_valid_file() -> None:
    """Every prefix of a valid file must be rejected.

    This is the cheapest way to cover a very large space of truncation bugs: a
    reader that trusts a length before checking it will accept some prefix.
    """
    base = (FIXTURE_DIR / "devices_fixture.nbdb").read_bytes()
    step = max(1, len(base) // 64)
    tested = 0
    for length in range(1, len(base), step):
        try:
            validator.validate(base[:length])
        except validator.Invalid:
            tested += 1
            continue
        except Exception as exc:  # noqa: BLE001
            check(False, f"prefix {length}: raised {type(exc).__name__}")
            continue
        check(False, f"prefix of {length} bytes was ACCEPTED")
    check(tested > 0, "truncation sweep actually ran")


def main() -> int:
    test_crc_and_hash_match_reference_values()
    test_determinism()
    test_provenance_gate_rejects_reference_only()
    test_recipe_rules_are_enforced()
    test_policy_and_identity_rules()
    test_string_table_limits()
    test_validator_accepts_committed_fixture()
    test_validator_rejects_every_variant()
    test_validator_rejects_truncations_of_valid_file()

    print(f"device_db_python: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
