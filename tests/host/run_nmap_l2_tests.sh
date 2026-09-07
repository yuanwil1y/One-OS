#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/one_os_nmap_l2_host_tests"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/tests/host/stubs" \
  -I"$ROOT/firmware/components/nmap_l2/include" \
  -I"$ROOT/firmware/components/nmap_l2" \
  "$ROOT/firmware/components/nmap_l2/nmap_core.c" \
  "$ROOT/firmware/components/nmap_l2/nmap_service.c" \
  "$ROOT/tests/host/test_nmap_l2.c" \
  -o "$OUT"
"$OUT"
