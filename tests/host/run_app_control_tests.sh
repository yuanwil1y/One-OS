#!/usr/bin/env bash
# Host regression tests for the unified control loop.
#
# app_control.c is the single path from "a user changed an Entity" to "the Entity's
# state changed", and the property it enforces - a successful send is not a state
# change - is a rule about ORDERING. So this group drives the real state machine with
# injected backends and asserts what the observed state is at each step: not only that
# a confirmation eventually arrives, but that nothing moves before it does.
#
# APP_DEVICE_TEST_HOOKS compiles the app_device test hooks in. They are guarded by that
# macro precisely so the firmware image never contains them.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_control_tests"
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
  "$ROOT/tests/host/test_app_control.c" \
  -o "$OUT"

"$OUT"
