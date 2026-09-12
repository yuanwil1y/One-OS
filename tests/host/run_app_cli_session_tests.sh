#!/usr/bin/env sh
# Host regression tests for the headless acceptance loop.
#
# tests/host/test_app_diag_protocol.c checks the parser and formatter as units.
# This group walks a whole session - the lines an operator types at the serial
# console while working through docs/hardware-acceptance.md - through the real
# parse -> decide -> render path, and asserts on the rendered text. The failure it
# is built to catch is drift: a command that parses but renders a response nobody
# can act on, a refusal whose reason changed, or a stage verdict that stopped being
# reported.
#
# The decisions use the real application modules (app_control for the control
# refusals, app_scan for the stage verdicts), not a second copy of the rules.
# What it cannot prove is the platform half: app_runtime.c, app_scan_native.c and
# the radio paths are ESP-IDF-only and are exercised by the target build.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_cli_session_tests"
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
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/main/app_str.c" \
  "$ROOT/firmware/main/app_ops.c" \
  "$ROOT/firmware/main/app_scan.c" \
  "$ROOT/firmware/main/device_db_format.c" \
  "$ROOT/firmware/main/app_recognition.c" \
  "$ROOT/firmware/main/app_device.c" \
  "$ROOT/firmware/main/app_control.c" \
  "$ROOT/firmware/main/app_diag_protocol.c" \
  "$ROOT/tests/host/stubs/app_l2_lookup_stub.c" \
  "$ROOT/tests/host/test_app_cli_session.c" \
  -o "$OUT"

"$OUT"
