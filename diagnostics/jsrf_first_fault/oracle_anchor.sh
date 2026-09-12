#!/bin/sh
# Run one host to a guest-clock anchor and dump the object inventory there.
#
# The whole point is that a differential sweep is MANY runs, so a run must cost
# exactly what reaching the anchor costs and not a second more:
#   * the periodic report is switched off (RECOMP_REPORT_MS huge) -- it is
#     expensive, and at 2 s it cost about 2.7x of guest throughput;
#   * the anchor is checked from the pushbuffer ack loop, not from the report,
#     so its precision does not depend on REPORT_MS (measured overshoot: 0);
#   * RECOMP_OBJECT_DUMP_EXIT ends the run the moment the dump is written.
#
# Usage: oracle_anchor.sh mac|win <clock> <outdir>
set -e
HOST="$1"; CLOCK="$2"; OUT="$3"
[ -n "$OUT" ] || { echo "usage: $0 mac|win <clock> <outdir>" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH="${ORACLE_SCRATCH:-/tmp/jsrf-oracle}"
rm -rf "$OUT" "$SCRATCH/hdd-$HOST-$CLOCK"; mkdir -p "$OUT" "$SCRATCH"
cp -R "$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd" \
      "$SCRATCH/hdd-$HOST-$CLOCK"
XBE="$ROOT/../Jet Set Radio Future (US)/default.xbe"
DIR="$ROOT/../Jet Set Radio Future (US)"
START=$(date +%s)
if [ "$HOST" = mac ]; then
  RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
  RECOMP_REPORT_MS=600000 RECOMP_FUNC_HIT_TRACE=1 \
  RECOMP_OBJECT_DUMP="$OUT" RECOMP_OBJECT_DUMP_AT="$CLOCK" \
  RECOMP_OBJECT_DUMP_EXIT=1 RECOMP_HDD_ROOT="$SCRATCH/hdd-$HOST-$CLOCK" \
    "$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault" \
    > "$OUT/stderr.log" 2>&1 || true
else
  B="$HOME/Library/Application Support/CrossOver/Bottles/recomp-gate/drive_c/jsrf"
  cp "$ROOT/build-macos/jsrf-first-fault/build-win-clang/jsrf_first_fault.exe" \
     "$B/jsrf_oracle.exe"
  W() { printf 'Z:%s' "$1" | tr '/' '\\'; }
  # printf, never echo: these lines are full of backslashes and sh's echo
  # eats them -- /tmp became a literal tab and \Users became a BEL before this
  # was noticed, and the guest just said "failed to load XBE".
  { printf '%s\n' '@echo off'
    printf '%s\n' 'set RECOMP_AC97_READY=1'
    printf '%s\n' 'set RECOMP_IRQ_THREAD=1'
    printf '%s\n' 'set RECOMP_PB_EXEC=1'
    printf '%s\n' 'set RECOMP_OHCI_ATTACH=1'
    printf '%s\n' 'set RECOMP_REPORT_MS=600000'
    printf '%s\n' 'set RECOMP_FUNC_HIT_TRACE=1'
    printf 'set RECOMP_OBJECT_DUMP=%s\n' "$(W "$OUT")"
    printf 'set RECOMP_OBJECT_DUMP_AT=%s\n' "$CLOCK"
    printf '%s\n' 'set RECOMP_OBJECT_DUMP_EXIT=1'
    printf 'C:\\jsrf\\jsrf_oracle.exe "%s" "%s" "%s"\n' \
      "$(W "$XBE")" "$(W "$DIR")" "$(W "$SCRATCH/hdd-$HOST-$CLOCK")"
  } > "$B/run_oracle.bat"
  "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
    --bottle recomp-gate --cx-app 'C:\jsrf\run_oracle.bat' \
    > "$OUT/stdout.log" 2> "$OUT/stderr.log" || true
fi
END=$(date +%s)
echo "$HOST clock=$CLOCK wall=$((END-START))s $(grep -oE 'overshoot [0-9]+' "$OUT/stderr.log" | head -1) dump=$(ls "$OUT"/*.json 2>/dev/null | wc -l | tr -d ' ')"
