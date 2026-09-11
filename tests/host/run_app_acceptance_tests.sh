#!/usr/bin/env bash
# Software acceptance for the headless (no-GUI) product chain.
#
# This group drives the real state machines together for many rounds and asserts the
# properties that only appear over time: bounded tables, accounted-for evictions, no
# orphans, honest truncation reporting, and the freshness rules under cancellation.
#
# It is NOT hardware acceptance. Every figure is a software bound; no heap, stack, RF or
# SD behaviour is measured here. The hardware checklist is docs/hardware-acceptance.md.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_acceptance_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DAPP_DEVICE_TEST_HOOKS \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/main" \
  -I"$ROOT/firmware/components/ha_core/include" \
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/firmware/main/app_control.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_acceptance.c" \
  -o "$OUT"

"$OUT"
