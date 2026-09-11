#!/usr/bin/env bash
# Host regression tests for the SD-backed recognition database reader.
#
# The real app_device_db.c is compiled here against a STUB STORAGE VTABLE, which
# is what makes the failure modes a real SD card can produce testable without a
# card: no medium, no file, a short read, an I/O error, a file truncated or
# rewritten underneath the reader, a corpus that changed length, an index too
# large to hold, and a version this reader does not implement.
#
# The corpus under test is the same committed .nbdb fixture the format group
# validates, regenerated first so the bytes are never stale.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
FIXTURE_DIR="$ROOT/tests/fixtures/device_db"
OUT="${TMPDIR:-/tmp}/one_os_app_device_db_tests"
CC_BIN="${CC:-cc}"
PYTHON="${PYTHON:-python3}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

echo "--- regenerating the fixture corpus ---"
"$PYTHON" "$ROOT/tools/device_db/build_device_db.py" >/dev/null

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DDEVICE_DB_FIXTURE_DIR="\"$FIXTURE_DIR\"" \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/components/ha_core/include" \
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_device_db.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_device_db.c" \
  -o "$OUT"

"$OUT"
