#!/usr/bin/env bash
# Host regression tests for the Device DB import state machine and recovery.
#
# app_db_import.c is platform independent: every filesystem operation arrives
# through a vtable, so this group drives the real algorithm against a fake FAT
# filesystem that can be "killed" after any operation. That is what makes the
# power-loss behaviour of the replacement sequence testable without a card - and
# the replacement sequence exists precisely because FAT rename is not atomic.
#
# No flash, no card, no ESP-IDF.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
FIXTURE_DIR="$ROOT/tests/fixtures/device_db"
OUT="${TMPDIR:-/tmp}/one_os_app_db_import_tests"
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
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_db_import.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_db_import.c" \
  -o "$OUT"

"$OUT"
