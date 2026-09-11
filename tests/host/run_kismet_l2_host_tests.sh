#!/usr/bin/env sh
# Kismet L2 bounded Wi-Fi/BLE tracker host regression tests.
#
# This is the exact command previously inlined in .github/workflows/build.yml,
# moved into a runner so CI and tests/run_all_host_tests.sh execute identically.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_kismet_l2_host_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/tools/kismet_l2_host_tests/stub" \
  -I"$ROOT/firmware/components/kismet_l2/include" \
  -I"$ROOT/firmware/components/kismet_l2" \
  "$ROOT/tools/kismet_l2_host_tests/kismet_l2_host_test.c" \
  "$ROOT/firmware/components/kismet_l2/kismet_wifi_tracker.c" \
  "$ROOT/firmware/components/kismet_l2/kismet_wifi_decode.c" \
  "$ROOT/firmware/components/kismet_l2/kismet_ble_tracker.c" \
  -o "$OUT"

"$OUT"
