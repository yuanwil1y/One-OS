#!/usr/bin/env sh
# Wireshark L2 bounded Wi-Fi/BLE parser host tests.
#
# This is the exact `make -C tests/host/wireshark_l2 test` target previously
# invoked from .github/workflows/build.yml, moved into a runner so CI and
# tests/run_all_host_tests.sh execute identically.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/tests/host/wireshark_l2" clean >/dev/null
make -C "$ROOT/tests/host/wireshark_l2" test
make -C "$ROOT/tests/host/wireshark_l2" clean >/dev/null
