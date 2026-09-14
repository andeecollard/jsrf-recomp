#!/usr/bin/env bash
# The translator tests on macOS.
#
# This used to begin with an unconditional
#     python3 -m pip install --user capstone pytest pefile numpy
# under `set -e`. On a current macOS that fails with
# externally-managed-environment and the script dies before running a single
# test -- so the one command a new contributor is told to run reported an
# install error and nothing about the code.
#
# It also ignored PYTHON, which matters here: capstone is installed for
# /usr/bin/python3 and not necessarily for whatever python3 is first on PATH.
#
# So: check what is available, run what can be run, and only mention installing
# if something is actually missing.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

PY="${PYTHON:-/usr/bin/python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
    PY=python3
fi
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "no python3 found. Set PYTHON to one, or install Python 3.10+." >&2
    exit 1
fi
echo "python: $PY ($("$PY" --version 2>&1))"

if ! "$PY" -c 'import capstone' >/dev/null 2>&1; then
    echo "capstone is not importable under $PY." >&2
    echo "  try:  PYTHON=/usr/bin/python3 $0" >&2
    echo "  or:   $PY -m pip install --user capstone" >&2
    exit 1
fi

# Run each test as a MODULE from the repo root. Several use relative imports
# (from .disasm import ...) and fail outright when run as bare scripts, which
# is a confusing way to discover that your invocation was wrong rather than
# the code. pytest is used when it is importable, and not required.
pass=0; fail=0; failed=""
for f in tools/recomp/test_*.py tools/disasm/test_*.py; do
    [ -e "$f" ] || continue
    mod="$(printf '%s' "${f%.py}" | tr '/' '.')"
    if "$PY" -m "$mod" >/dev/null 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); failed="$failed $(basename "$f")"
    fi
done

echo "$pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    echo "failed:$failed" >&2
    echo "re-run one for the detail:  $PY -m tools.recomp.<name>" >&2
    exit 1
fi
