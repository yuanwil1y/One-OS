#!/usr/bin/env bash
#
# Unified host test entry point for One-OS.
#
# This runs every existing host regression group by invoking the original
# per-group scripts. It deliberately does not re-implement or copy any test:
# each group keeps its own runner so CI, a developer shell and this aggregate
# entry point always execute identical commands.
#
# Usage:
#   tests/run_all_host_tests.sh            # run every group
#   tests/run_all_host_tests.sh --list     # list group names only
#
# Environment:
#   LEAK_SANITIZER=0   disable LeakSanitizer for groups that use ASan.
#                      Needed in containers where LeakSanitizer cannot read
#                      /proc/<pid>/task. The repository tests are NOT modified;
#                      only the sanitizer environment is adjusted per run.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIST_ONLY=0

for arg in "$@"; do
  case "$arg" in
    --list) LIST_ONLY=1 ;;
    -h|--help)
      sed -n '2,20p' "$0"
      exit 0
      ;;
    *)
      echo "unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

# name|command
GROUPS=(
  "ha_l2|tests/run_ha_l2_host_tests.sh"
  "esphome_l2|tests/esphome_l2/run_host_tests.sh"
  "theengs_l2|firmware/components/theengs_l2/tests/run_host_tests.sh"
  "zha_zigpy_l2|tests/host/run_zha_zigpy_host_tests.sh"
  "openthread_l2|tests/run_openthread_l2_host_test.sh"
  "kismet_l2|tests/host/run_kismet_l2_host_tests.sh"
  "nmap_l2|tests/host/run_nmap_l2_tests.sh"
  "wireshark_l2|tests/host/run_wireshark_l2_tests.sh"
  "app_diag_protocol|tests/host/run_app_diag_protocol_tests.sh"
)

if [ "$LIST_ONLY" -eq 1 ]; then
  for group in "${GROUPS[@]}"; do
    echo "${group%%|*}"
  done
  exit 0
fi

if [ "${LEAK_SANITIZER:-1}" = "0" ]; then
  # Passing an explicit detect_leaks=0 keeps ASan/UBSan active; only the leak
  # check is suppressed, and this is reported in the summary.
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
  echo "note: LeakSanitizer disabled via LEAK_SANITIZER=0 (ASan/UBSan still active)"
fi

pass=0
fail=0
failed_groups=()

for group in "${GROUPS[@]}"; do
  name="${group%%|*}"
  cmd="${group#*|}"
  runner="$ROOT/$cmd"

  printf '\n=== host test group: %s ===\n' "$name"

  if [ ! -f "$runner" ]; then
    printf 'MISSING runner: %s\n' "$cmd"
    fail=$((fail + 1))
    failed_groups+=("$name (missing runner)")
    continue
  fi

  if ( cd "$ROOT" && bash "$cmd" ); then
    printf -- '--- %s: PASS\n' "$name"
    pass=$((pass + 1))
  else
    printf -- '--- %s: FAIL\n' "$name"
    fail=$((fail + 1))
    failed_groups+=("$name")
  fi
done

printf '\n================ host test summary ================\n'
printf 'passed groups: %d\n' "$pass"
printf 'failed groups: %d\n' "$fail"
if [ "$fail" -gt 0 ]; then
  printf 'failed:\n'
  for name in "${failed_groups[@]}"; do
    printf '  - %s\n' "$name"
  done
  exit 1
fi
printf 'all host test groups passed\n'
