#!/usr/bin/env bash
# Host tests for the Python half of the Device DB tooling.
#
# The generator's own rules (determinism, the provenance gate, cross-field
# validation) and the independent validator are exercised here. The C reader has
# its own group; both run against the same fixture files.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Prefer an explicit interpreter, then python3, then python. CI provides
# python3; a developer shell on Windows may only have python.
if [ -n "${PYTHON:-}" ]; then
  PY="$PYTHON"
elif command -v python3 >/dev/null 2>&1; then
  PY=python3
else
  PY=python
fi

echo "--- interpreter: $PY ($($PY --version 2>&1)) ---"

# Regenerate first so the tests always run against a fresh, self-consistent
# fixture rather than a stale committed one.
"$PY" "$ROOT/tools/device_db/build_device_db.py" >/dev/null
"$PY" "$ROOT/tools/device_db/make_invalid_fixtures.py" >/dev/null

"$PY" "$ROOT/tools/device_db/test_device_db_python.py"
