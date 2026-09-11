#!/usr/bin/env bash
# Host tests for the Device DB (.nbdb) format: generator, validator and reader.
#
# Three independent checks, in the order that makes a failure easy to localise:
#
#   1. the Python generator is reproducible and the Python validator accepts the
#      fixture it produced (and rejects every damaged variant);
#   2. the C reader -- the code the firmware actually runs -- accepts the same
#      fixture and decodes the expected records;
#   3. the C reader rejects every one of the deliberately damaged variants.
#
# Step 1 and step 2 use the same files on disk, so a disagreement between the
# Python side and the C side fails here rather than on hardware.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXTURE_DIR="$ROOT/tests/fixtures/device_db"
OUT="${TMPDIR:-/tmp}/one_os_device_db_format_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

PYTHON="${PYTHON:-python3}"

echo "--- regenerating fixtures from the committed sources ---"
"$PYTHON" "$ROOT/tools/device_db/build_device_db.py"
"$PYTHON" "$ROOT/tools/device_db/make_invalid_fixtures.py" >/dev/null

echo "--- generator output must be reproducible ---"
"$PYTHON" "$ROOT/tools/device_db/build_device_db.py" --check

echo "--- python validator accepts the fixture ---"
"$PYTHON" "$ROOT/tools/device_db/validate_device_db.py" \
  "$FIXTURE_DIR/devices_fixture.nbdb"

echo "--- python validator rejects every damaged variant ---"
rejected=0
while IFS= read -r variant; do
  [ -n "$variant" ] || continue
  "$PYTHON" "$ROOT/tools/device_db/validate_device_db.py" \
    "$FIXTURE_DIR/invalid/$variant" --expect-failure >/dev/null
  rejected=$((rejected + 1))
done < "$FIXTURE_DIR/invalid/manifest.txt"
echo "  python rejected $rejected variants"

echo "--- building and running the C reader tests ---"
"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DDEVICE_DB_FIXTURE_DIR="\"$FIXTURE_DIR\"" \
  -I"$ROOT/firmware/main/include" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/tests/host/test_device_db_format.c" \
  -o "$OUT"

"$OUT"
