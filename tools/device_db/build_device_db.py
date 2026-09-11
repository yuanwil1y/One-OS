#!/usr/bin/env python3
"""Build the One-OS Device DB (.nbdb) test fixture corpus.

Input:  tools/device_db/sources/source_manifest.json
        tools/device_db/profiles/fixture_profiles.json
Output: tests/fixtures/device_db/devices_fixture.nbdb
        tests/fixtures/device_db/devices_fixture.sha256   (recorded digest)

The build is deterministic: the same inputs produce byte-identical output. The
recorded digest is committed so a non-deterministic change fails the host test
instead of silently altering the corpus.

This generator is a host tool. It never runs on the device and its output is
never compiled into firmware: the production recognition corpus lives only on
the SD card at /nearby/db/devices.nbdb.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import nbdb  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCES = ROOT / "tools" / "device_db" / "sources" / "source_manifest.json"
PROFILES = ROOT / "tools" / "device_db" / "profiles" / "fixture_profiles.json"
OUT_DIR = ROOT / "tests" / "fixtures" / "device_db"
OUT_DB = OUT_DIR / "devices_fixture.nbdb"
OUT_SHA = OUT_DIR / "devices_fixture.sha256"

CONTENT_VERSION = 20260911


def load_manifest(path: pathlib.Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def profile_from_json(raw: dict) -> nbdb.Profile:
    provenance = nbdb.Provenance(**raw["provenance"])
    fingerprints = [
        nbdb.Fingerprint(
            protocol=item["protocol"],
            key=item["key"],
            match_kind=item.get("match_kind", "exact"),
            mask=item.get("mask", ""),
            flags=item.get("flags", 0),
        )
        for item in raw.get("fingerprints", [])
    ]
    recipes = [
        nbdb.Recipe(
            domain=item["domain"],
            name=item.get("name", ""),
            device_class=item.get("device_class", ""),
            unit=item.get("unit", ""),
            backend=item.get("backend", "none"),
            read_source_id=item.get("read_source_id", nbdb.NO_INDEX),
            write_target_id=item.get("write_target_id", nbdb.NO_INDEX),
            codec_id=item.get("codec_id", nbdb.NO_INDEX),
            subscription_id=item.get("subscription_id", nbdb.NO_INDEX),
            endpoint=item.get("endpoint", nbdb.NO_ENDPOINT),
            cluster=item.get("cluster", nbdb.NO_ENDPOINT),
            attribute=item.get("attribute", nbdb.NO_ENDPOINT),
            command=item.get("command", nbdb.NO_ENDPOINT),
            min_value=item.get("min_value", 0),
            max_value=item.get("max_value", 0),
            scale=item.get("scale", 0),
            flags=item.get("flags", 0),
        )
        for item in raw.get("recipes", [])
    ]
    identity_rules = [
        nbdb.IdentityRule(
            kind=item["kind"],
            strength=item["strength"],
            key=item.get("key", ""),
            flags=item.get("flags", 0),
        )
        for item in raw.get("identity_rules", [])
    ]
    return nbdb.Profile(
        profile_id=raw["profile_id"],
        vendor=raw.get("vendor", ""),
        model=raw.get("model", ""),
        display_name=raw.get("display_name", ""),
        icon=raw.get("icon", ""),
        provenance=provenance,
        fingerprints=fingerprints,
        recipes=recipes,
        identity_rules=identity_rules,
        theengs_decoder_id=raw.get("theengs_decoder_id", nbdb.NO_INDEX),
        zha_quirk_id=raw.get("zha_quirk_id", nbdb.NO_INDEX),
        policy=raw.get("policy", []),
        writable=raw.get("writable", False),
    )


def build_image() -> bytes:
    manifest = load_manifest(SOURCES)
    sources = manifest["sources"]
    known = [s["source_id"] for s in sources]
    # A source is reference-only for output purposes when either its reuse class
    # says so or a review has not permitted output yet.
    reference_only = [
        s["source_id"] for s in sources
        if s["reuse_class"] == "reference_only" or not s.get("permitted_for_output", False)
    ]

    raw_profiles = load_manifest(PROFILES)["profiles"]
    profiles = [profile_from_json(item) for item in raw_profiles]

    return nbdb.build(
        profiles,
        content_version=CONTENT_VERSION,
        # 0 keeps the build reproducible.
        build_timestamp=0,
        known_source_ids=known,
        reference_only_sources=reference_only,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify the committed fixture matches a fresh build")
    parser.add_argument("--print-digest", action="store_true",
                        help="build and print the digest without writing")
    args = parser.parse_args()

    try:
        image = build_image()
    except nbdb.DeviceDbError as exc:
        print(f"generator refused to build: {exc}", file=sys.stderr)
        return 1

    digest = hashlib.sha256(image).hexdigest()

    if args.print_digest:
        print(digest)
        return 0

    if args.check:
        if not OUT_DB.exists():
            print(f"missing fixture {OUT_DB}; run without --check first",
                  file=sys.stderr)
            return 1
        actual = OUT_DB.read_bytes()
        if actual != image:
            print(
                "fixture is not reproducible: a fresh build differs from the "
                "committed file",
                file=sys.stderr,
            )
            return 1
        recorded = OUT_SHA.read_text(encoding="utf-8").strip().split()[0]
        if recorded != digest:
            print(
                f"recorded digest {recorded} does not match the build {digest}",
                file=sys.stderr,
            )
            return 1
        print(f"reproducible: {digest}")
        return 0

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DB.write_bytes(image)
    OUT_SHA.write_text(
        f"{digest}  {OUT_DB.name}\n"
        f"# Generated by tools/device_db/build_device_db.py from\n"
        f"#   tools/device_db/sources/source_manifest.json\n"
        f"#   tools/device_db/profiles/fixture_profiles.json\n"
        f"# This file is a TEST FIXTURE. It is not compiled into firmware and is\n"
        f"# not the production recognition corpus, which lives on SD.\n",
        encoding="utf-8",
    )
    print(f"wrote {OUT_DB.relative_to(ROOT)} ({len(image)} bytes)")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
