#!/usr/bin/env sh
# Host regression tests for the application Device/Entity binding table.
#
# app_device.c is compiled against the real ha_core, so these tests exercise the
# production identity, dedup, generation, sweep and read-only rules rather than a
# mock of them.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_device_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/components/ha_core/include" \
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/tests/host/test_app_device.c" \
  -o "$OUT"

"$OUT"
