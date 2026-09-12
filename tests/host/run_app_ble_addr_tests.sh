#!/usr/bin/env sh
# Host regression tests for the BLE address byte-order convention.
#
# Whether the application holds addresses least-significant-byte-first (what the
# radio reports) or most-significant-byte-first (what every user-facing surface
# prints). Holding the wrong one makes app_device's generated device id read
# backwards and makes a comparison against a database's BLE_PUBLIC_ADDRESS key a
# comparison of two different encodings.
#
# The dangerous failure is a conversion applied twice, which is byte-for-byte
# identical to none - so the vectors here are anchored to a real address written
# both ways rather than to the module's own output, and both directions plus the
# round trip are pinned.
#
# Whether the scan path applies it exactly once is proven by app_ble_addr_handoff
# below; whether a real controller accepts the result is item 5b.2 of
# docs/hardware-acceptance.md.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/one_os_app_ble_addr_tests"
CC_BIN="${CC:-cc}"
trap 'rm -f "$OUT"' EXIT HUP INT TERM

"$CC_BIN" \
  -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/firmware/main/include" \
  "$ROOT/firmware/main/app_ble_addr.c" \
  "$ROOT/tests/host/test_app_ble_addr.c" \
  -o "$OUT"

"$OUT"
