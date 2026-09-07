#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="${TMPDIR:-/tmp}/one-os-ha-l2-host-tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/firmware/components/ha_core/include" \
  -I"$ROOT/firmware/components/ha_discovery_l2/include" \
  -I"$ROOT/firmware/components/ha_discovery_l2" \
  "$ROOT/tests/ha_l2_host_tests.c" \
  "$ROOT/firmware/components/ha_core/ha_core.c" \
  "$ROOT/firmware/components/ha_discovery_l2/ha_mdns_parser.c" \
  "$ROOT/firmware/components/ha_discovery_l2/ha_ssdp_parser.c" \
  -o "$OUT"

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$OUT"
