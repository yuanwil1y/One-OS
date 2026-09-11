#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
INC="-I$ROOT/firmware/components/esphome_l2/include -I$ROOT/firmware/components/esphome_l2"
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O2"
mkdir -p "$ROOT/tests/esphome_l2/build"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_noise_crypto.c" "$ROOT/tests/esphome_l2/test_noise_crypto.c" -o "$ROOT/tests/esphome_l2/build/test_noise_crypto"
"$ROOT/tests/esphome_l2/build/test_noise_crypto"
