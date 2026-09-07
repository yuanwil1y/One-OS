#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/openthread_l2_host_test
"$CC" -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/firmware/components/openthread_l2/include" \
  -I"$ROOT/firmware/components/openthread_l2" \
  "$ROOT/firmware/components/openthread_l2/openthread_l2_util.c" \
  "$ROOT/tests/openthread_l2_host_test.c" \
  -o "$OUT"
"$OUT"
