#!/usr/bin/env bash
#
# Differential conformance on macOS.
#
# The suite proves the lifter correct by executing each snippet as real 32-bit
# x86 and comparing that against the lifted C. macOS has no such CPU to offer so a
# linux/386 container supplies the toolchain and the CPU while the lifting stays
# here on the host. What is substituted is the toolchain, never the comparison.
#
# The corpus and XBE phases stay Windows-only (they link a PE DLL and lift it
# back out of the linked image), so this runs the snippet phase.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

IMAGE=xboxrecomp-conf-i386

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required on macOS. Install Python 3.10+ and re-run this script." >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "Docker must be installed and running: it supplies the 32-bit x86 CPU" >&2
    echo "that the lifted C is compared against." >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "The linux/386 oracle image is missing; building it once (a minute or two)." >&2
    if ! docker build --platform linux/386 -t "$IMAGE" -f Dockerfile . >&2; then
        echo >&2
        echo "Building the oracle image failed. Without it there is no 32-bit x86" >&2
        echo "to compare the lifted C against, so nothing here can be verified." >&2
        exit 1
    fi
    echo "Built $IMAGE." >&2
fi

echo "Running conformance tests in a linux/386 container..."

python3 -m pip install --user capstone
python3 -m tools.conformance --only snippets "$@"
