#!/usr/bin/env bash
# Host regression tests for the BLE GATT session.
#
# The GATT layer below this module is a synchronous wrapper with one shared
# completion slot and no session identity, so a cancel or a deadline can make an
# abandoned operation return success and a queued notification can arrive after
# teardown. Those are the cases this group drives, with a scripted backend that
# reproduces the exact orderings: a backend that answers ESP_OK for a cancelled
# operation, a deadline that passes under an in-flight read, and a notification
# delivered through a bridge that belongs to a previous session.
#
# app_ble_gatt.c is platform independent; the firmware supplies an adapter over
# the esphome_l2 GATT component.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ble_gatt_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/main" \
  -I"$ROOT/firmware/components/esphome_l2/include" \
  "$ROOT/firmware/main/app_ble_gatt.c" \
  "$ROOT/tests/host/test_app_ble_gatt.c" \
  -o "$OUT"

"$OUT"
