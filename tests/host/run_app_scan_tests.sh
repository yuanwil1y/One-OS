#!/usr/bin/env sh
# Host regression tests for the application scan policy and bounded evidence.
#
# The same app_scan.c source is compiled into the ESP32-C6 firmware, so these
# tests pin the stage decisions (skip/unavailable/rejected), the RF stage verdict
# used for session-teardown fault injection, and the evidence merge rules without
# hardware.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_scan_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/tests/host/test_app_scan.c" \
  -o "$OUT"

"$OUT"
