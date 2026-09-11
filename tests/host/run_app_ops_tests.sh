#!/usr/bin/env sh
# Host regression tests for the application operation gate and scan lifecycle.
#
# The same app_ops.c source is compiled into the ESP32-C6 firmware, so these
# tests verify BUSY handling, cancellation and terminal-state reporting without
# hardware.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ops_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/tests/host/test_app_ops.c" \
  -o "$OUT"

"$OUT"
