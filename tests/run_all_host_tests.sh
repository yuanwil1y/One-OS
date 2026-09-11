#!/usr/bin/env bash
#
# Unified host test entry point for One-OS.
#
# Runs every host regression group by invoking the original per-group runners
# listed in tests/host-test-groups.txt. It deliberately does not re-implement or
# copy any test: each group keeps its own runner, so CI, a developer shell and
# this aggregate entry point always execute identical commands.
#
# The group list is a plain newline-delimited manifest rather than a shell
# array. That keeps the dispatcher readable, diffable, and independent of any
# shell-version-specific parsing.
#
# Usage:
#   tests/run_all_host_tests.sh                 # run every group
#   tests/run_all_host_tests.sh --list          # list group names only
#   tests/run_all_host_tests.sh --group NAME    # run one group
#
# Environment:
#   LEAK_SANITIZER=0   disable the LeakSanitizer leak check for groups that use
#                      ASan. Needed in containers where LeakSanitizer cannot
#                      read /proc/<pid>/task. ASan/UBSan stay active and no
#                      repository test is modified or disabled.
#
set -uo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$ROOT/tests/host-test-groups.txt"

LIST_ONLY=0
ONLY_GROUP=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --list)
      LIST_ONLY=1
      shift
      ;;
    --group)
      if [ "$#" -lt 2 ]; then
        echo "--group requires a group name" >&2
        exit 2
      fi
      ONLY_GROUP="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,26p' "$0"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [ ! -f "$MANIFEST" ]; then
  echo "missing group manifest: $MANIFEST" >&2
  exit 2
fi

if [ "$LIST_ONLY" -eq 1 ]; then
  while read -r name _command; do
    [ -n "$name" ] || continue
    case "$name" in '#'*) continue ;; esac
    echo "$name"
  done < "$MANIFEST"
  exit 0
fi

if [ "${LEAK_SANITIZER:-1}" = "0" ]; then
  # An explicit detect_leaks=0 keeps ASan/UBSan active; only the leak check is
  # suppressed, and that fact is reported in the summary below.
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
  echo "note: LeakSanitizer disabled via LEAK_SANITIZER=0 (ASan/UBSan still active)"
fi

passed=0
failed=0
failed_names=""
selected=0

while read -r name command; do
  [ -n "$name" ] || continue
  case "$name" in '#'*) continue ;; esac

  if [ -n "$ONLY_GROUP" ] && [ "$name" != "$ONLY_GROUP" ]; then
    continue
  fi
  selected=$((selected + 1))

  runner="$ROOT/$command"
  printf '\n=== host test group: %s ===\n' "$name"

  if [ ! -f "$runner" ]; then
    printf 'MISSING runner: %s\n' "$command"
    failed=$((failed + 1))
    failed_names="$failed_names $name(missing-runner)"
    continue
  fi

  if ( cd "$ROOT" && bash "$command" ); then
    printf -- '--- %s: PASS\n' "$name"
    passed=$((passed + 1))
  else
    printf -- '--- %s: FAIL\n' "$name"
    failed=$((failed + 1))
    failed_names="$failed_names $name"
  fi
done < "$MANIFEST"

if [ "$selected" -eq 0 ]; then
  echo "no host test group selected" >&2
  exit 2
fi

printf '\n================ host test summary ================\n'
printf 'passed groups: %d\n' "$passed"
printf 'failed groups: %d\n' "$failed"

if [ "$failed" -gt 0 ]; then
  printf 'failed:%s\n' "$failed_names"
  exit 1
fi

printf 'all host test groups passed\n'
