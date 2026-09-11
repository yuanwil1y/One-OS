#!/usr/bin/env bash
# Host regression tests for the provisioning session.
#
# app_provision.c owns the ordering: the operation gate, the radio handover, and the
# window in which the recognition reader is closed so the corpus can be replaced.
# Ordering is what this group asserts, with an injected platform and an ordered call
# log - a final-state check cannot see "was the radio released before the AP was
# configured".
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_provision_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/components/ha_core/include" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_db_import.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_portal.c" \
  "$ROOT/firmware/main/app_provision.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_provision.c" \
  -o "$OUT"

"$OUT"
