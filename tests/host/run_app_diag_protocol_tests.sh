#!/usr/bin/env sh
# Host regression tests for the headless diagnostic command surface.
#
# The same app_diag_protocol.c source is compiled into the ESP32-C6 firmware, so
# this test proves request correlation and response shape without hardware.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_diag_protocol_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  "$ROOT/firmware/main/app_diag_protocol.c" \
  "$ROOT/tests/host/test_app_diag_protocol.c" \
  -o "$OUT"

"$OUT"
