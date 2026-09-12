#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
INC="-I$ROOT/tests/esphome_l2/stubs -I$ROOT/tests/esphome_l2 -I$ROOT/firmware/components/esphome_l2/include -I$ROOT/firmware/components/esphome_l2"
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O2"
mkdir -p "$ROOT/tests/esphome_l2/build"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_ble_gatt.c" "$ROOT/tests/esphome_l2/test_gatt.c" -o "$ROOT/tests/esphome_l2/build/test_gatt"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_api_codec.c" "$ROOT/tests/esphome_l2/test_codec.c" -lm -o "$ROOT/tests/esphome_l2/build/test_codec"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_noise_crypto.c" "$ROOT/tests/esphome_l2/test_noise_crypto.c" -o "$ROOT/tests/esphome_l2/build/test_noise_crypto"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_noise_crypto.c" "$ROOT/firmware/components/esphome_l2/esphome_noise.c" "$ROOT/tests/esphome_l2/noise_test_responder.c" "$ROOT/tests/esphome_l2/test_noise.c" -o "$ROOT/tests/esphome_l2/build/test_noise"
cc $CFLAGS $INC "$ROOT/firmware/components/esphome_l2/esphome_api.c" "$ROOT/firmware/components/esphome_l2/esphome_api_codec.c" "$ROOT/firmware/components/esphome_l2/esphome_noise.c" "$ROOT/firmware/components/esphome_l2/esphome_noise_crypto.c" "$ROOT/tests/esphome_l2/noise_test_responder.c" "$ROOT/tests/esphome_l2/test_api_client.c" -pthread -o "$ROOT/tests/esphome_l2/build/test_api_client"
"$ROOT/tests/esphome_l2/build/test_gatt"
"$ROOT/tests/esphome_l2/build/test_codec"
"$ROOT/tests/esphome_l2/build/test_noise_crypto"
"$ROOT/tests/esphome_l2/build/test_noise"
"$ROOT/tests/esphome_l2/build/test_api_client"
