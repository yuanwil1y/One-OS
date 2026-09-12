#!/usr/bin/env bash
# Host regression tests for the ESPHome control backend.
#
# The same property as the BLE backend, and on ESPHome it is easier to get wrong: a
# state report arrives on the same subscription for an entity the device already had a
# value for, so confirming on "a report arrived" would mark a refused command as
# confirmed. Every assertion is therefore about ORDER and about WHICH state arrived.
#
# The API client is scripted. The wire is not exercised here: esphome_l2 has its own
# groups for the framing, the Noise handshake and the protobuf codec, and the
# interaction with a real node is item 5c of docs/hardware-acceptance.md.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ctl_esphome_tests"
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
  "$ROOT/firmware/main/app_ctl_esphome.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_ctl_esphome.c" \
  -o "$OUT"

"$OUT"
