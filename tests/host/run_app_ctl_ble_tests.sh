#!/usr/bin/env bash
# Host regression tests for the BLE GATT control backend.
#
# The property this group protects is the one the whole control design rests on: a
# successful send is not a state change. The backend writes to a characteristic and
# reports SENT; the observed state moves only when the device answers. A backend
# that confirmed its own write would make "confirmed" mean "we asked", and every
# surface - console, HTTP, the future GUI - would show a state nobody observed.
#
# The assertions are therefore about ORDER: what the observed state is after the
# send, after the write, and only then after the device reports. The GATT session is
# scripted, so each of those moments is reached deliberately instead of racing a
# radio.
#
# Two refusals are pinned as well, because both are silent on hardware: a control
# with no live link is FAILED rather than SENT (a control the caller is told left
# the firmware, which never left it, would sit pending until its deadline and then
# be reported as a device timeout), and an action the codec cannot encode is
# UNSUPPORTED rather than guessed.
#
# APP_DEVICE_TEST_HOOKS compiles in the binding hook this test needs; it is guarded
# by that macro so the firmware image never contains it, and it exists so a control
# test does not have to materialise a whole evidence set first.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ctl_ble_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DAPP_DEVICE_TEST_HOOKS \
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
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_ctl_ble.c" \
  -o "$OUT"

"$OUT"
