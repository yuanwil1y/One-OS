#!/usr/bin/env bash
# Host regression tests for the firmware binding from the control backend to the GATT
# session.
#
# app_ctl_ble has its own group proving the control policy against a scripted session.
# What it cannot prove is that the table the FIRMWARE supplies answers the one question
# only the firmware can: which characteristic, of which device.
#
# That is where a control path does its worst damage when it is wrong. A refusal is
# visible - the control fails and the reason is recorded. A WRONG HANDLE IS NOT: the
# write succeeds, the device acknowledges it, and the wrong attribute has been changed.
# So the refusals are what this group tests hardest, each reached deliberately: no live
# session, a different peer, a device id that is not a BLE one, an index with no value
# handle, and a characteristic that is not writable.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ctl_ble_gatt_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/tests/host" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/main" \
  -I"$ROOT/firmware/components/ha_core/include" \
  -I"$ROOT/firmware/components/esphome_l2/include" \
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/firmware/main/app_control.c" \
  "$ROOT/firmware/main/app_ble_addr.c" \
  "$ROOT/firmware/main/app_ble_gatt.c" \
  "$ROOT/firmware/main/app_ctl_ble.c" \
  "$ROOT/firmware/main/app_ctl_ble_gatt.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_ctl_ble_gatt.c" \
  -o "$OUT"

"$OUT"
