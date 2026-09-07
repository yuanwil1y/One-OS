#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=c11 -Wall -Wextra -Werror -pedantic"}
LDFLAGS=${LDFLAGS:-}
OUT=${TMPDIR:-/tmp}/theengs_l2_host_test
# CFLAGS/LDFLAGS are intentionally word-split to allow sanitizer flags.
$CC $CFLAGS \
    -I"$ROOT/include" \
    "$ROOT/theengs_l2.c" \
    "$ROOT/theengs_ruuvi.c" \
    "$ROOT/theengs_bthome.c" \
    "$ROOT/tests/test_theengs_l2.c" \
    $LDFLAGS -lm -o "$OUT"
"$OUT"
