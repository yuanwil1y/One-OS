#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${TMPDIR:-/tmp}/one-os-zha-zigpy-host-tests"
rm -rf "$BUILD"
mkdir -p "$BUILD"

CC_BIN="${CC:-cc}"
COMMON=(-std=c11 -Wall -Wextra -Werror -pedantic)

"$CC_BIN" "${COMMON[@]}" \
  -I"$ROOT/firmware/components/zigpy_l2/include" \
  "$ROOT/firmware/components/zigpy_l2/zigpy_l2.c" \
  "$ROOT/tests/host/test_zigpy_l2.c" \
  -o "$BUILD/test_zigpy_l2"
"$BUILD/test_zigpy_l2"

"$CC_BIN" "${COMMON[@]}" \
  -I"$ROOT/firmware/components/zha_l2/include" \
  "$ROOT/firmware/components/zha_l2/zha_l2.c" \
  "$ROOT/tests/host/test_zha_l2.c" \
  -o "$BUILD/test_zha_l2"
"$BUILD/test_zha_l2"

if grep -R -n -E '#include[[:space:]]+[<"]zha_l2|\bzha_[A-Za-z0-9_]*' \
    "$ROOT/firmware/components/zigpy_l2"; then
  echo "zigpy_l2 must not depend on zha_l2" >&2
  exit 1
fi
if grep -R -n -E '#include[[:space:]]+[<"]zigpy_l2|\bzigpy_[A-Za-z0-9_]*' \
    "$ROOT/firmware/components/zha_l2"; then
  echo "zha_l2 must not depend on zigpy_l2" >&2
  exit 1
fi

echo "zha/zigpy family-boundary checks passed"
