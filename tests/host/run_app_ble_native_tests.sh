#!/usr/bin/env bash
# Host regression tests for the firmware's BLE GATT adapter.
#
# app_ble_gatt owns the session policy and has its own group. This group covers the
# other half of that boundary: the ops table the FIRMWARE supplies, built on the
# real esphome_l2 transport. Only the radio is fake - esphome_ble_gatt_nimble.c is
# replaced by tests/host/fake_ble_transport.c, which implements the same ops table
# the NimBLE backend does - so the real framing, the real epoch guard, the real
# subscription table and the real radio suspend/resume points are all exercised.
#
# tests/host/fake_ble_backend_decl.h, force-included below, is what selects the
# scripted radio: it defines ESPHOME_BLE_GATT_BACKEND - the documented substitution
# point in firmware/components/esphome_l2/private/esphome_ble_gatt_internal.h -
# before any project header is read. The transport source is compiled unchanged;
# only the symbol its init installs changes. Overriding esphome_ble_gatt_init()
# instead would mean not compiling the real one, which would defeat the purpose.
#
# The four claims pinned here fail only on hardware if they are wrong: a failed
# read that leaves stale bytes in the caller's buffer, a last_error that reports
# the wrong operation, a radio pair that never reaches the transport or reports
# success for arbitration that did not happen, and a notification whose truncation
# flag is lost on the way up.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ble_native_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=gnu11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -include "$ROOT/tests/host/fake_ble_backend_decl.h" \
  -I"$ROOT/tests/host/ble_stubs" \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/tests/host" \
  -I"$ROOT/firmware/main/include" \
  -I"$ROOT/firmware/main" \
  -I"$ROOT/firmware/components/esphome_l2/include" \
  -I"$ROOT/firmware/components/esphome_l2" \
  "$ROOT/firmware/components/esphome_l2/esphome_ble_gatt.c" \
  "$ROOT/firmware/main/app_ble_gatt.c" \
  "$ROOT/firmware/main/app_ble_gatt_native.c" \
  "$ROOT/tests/host/fake_ble_transport.c" \
  "$ROOT/tests/host/test_app_ble_native.c" \
  -o "$OUT"

"$OUT"
