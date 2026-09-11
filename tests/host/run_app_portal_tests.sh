#!/usr/bin/env bash
# Host regression tests for the provisioning portal's wire layer.
#
# app_portal.c decides what an HTTP request MEANS and what a response REVEALS, and
# neither of those needs a socket. So the two properties that matter are pinned
# here: every bound is enforced before a value is used, and no response can carry a
# secret or be broken by a hostile SSID.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_portal_tests"
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
  "$ROOT/firmware/main/app_portal.c" \
  "$ROOT/tests/host/test_app_portal.c" \
  -o "$OUT"

"$OUT"
