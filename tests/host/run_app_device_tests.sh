#!/usr/bin/env sh
# Host regression tests for the application Device/Entity binding table.
#
# app_device.c is compiled against the real ha_core and the real recognition
# policy, so these tests exercise the production identity, dedup, generation,
# sweep and read-only rules rather than a mock of them. Recognition itself is
# supplied by the test as a stub recognizer, which is what keeps this group free
# of any filesystem dependency.
#
# app_recognition.c asks the Theengs and ZHA families which decoder/quirk ids they
# carry. Those two components are not otherwise built here, so
# tests/host/stubs/app_l2_lookup_stub.c supplies the answers for the ids the
# fixture corpus names; see tests/host/stubs/README.md.
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
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_device.c" \
  -o "$OUT"

"$OUT"
