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
# ORACLE_FB_DUMP=1 writes flipNNN.bmp into the output directory at each of the
# first frame boundaries. It is how the two renderers are compared as PICTURES
# rather than as triangle counts, which is the only check that catches a
# fragment program that is fast and wrong. Uses the live surface at the flip,
# not s_snap: s_snap is only maintained once a window has asked for it, and an
# anchored run has no window.
# Exported rather than written inline: `VAR= cmd` sets an EMPTY variable, and
# every switch in this harness is tested with getenv(), which sees "" as set.
if [ -n "$ORACLE_FB_DUMP" ]; then
  FBWIN=$(printf 'Z:%s/' "$OUT" | tr '/' '\\')
  RECOMP_FB_DUMP="$OUT/"; RECOMP_FB_DUMP_FLIP=1
  export RECOMP_FB_DUMP RECOMP_FB_DUMP_FLIP
fi
# ORACLE_FLIP_TRACE=<stride>: what each surface holds AT the flip, in guest RAM
# and -- where a GPU path is running -- on the GPU, for the same addresses.
if [ -n "$ORACLE_FLIP_TRACE" ]; then
  RECOMP_FLIP_TRACE="$ORACLE_FLIP_TRACE"
  export RECOMP_FLIP_TRACE
fi
# ORACLE_CONTIG_VERIFY=1: is the surface address we resolved the same storage
# as the contiguous-window alias of it? A physical framebuffer address read
# directly lands in the loaded image instead.
if [ -n "$ORACLE_CONTIG_VERIFY" ]; then
  RECOMP_CONTIG_VERIFY=1
  export RECOMP_CONTIG_VERIFY
fi
if [ "$HOST" = mac ]; then
  RECOMP_USB=${ORACLE_USB:-} \
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
    [ -n "$ORACLE_USB" ] && printf '%s\n' 'set RECOMP_USB=1'
    # ORACLE_D3D11=1 puts the batches on the GPU, which is what macOS does by
    # default above. Off by default so the two Windows configurations can be
    # anchored against each other at the same guest clock.
    [ -n "$ORACLE_D3D11" ] && printf '%s\n' 'set RECOMP_D3D11=1'
    [ -n "$ORACLE_FB_DUMP" ] && printf 'set RECOMP_FB_DUMP=%s\n' "$FBWIN"
    [ -n "$ORACLE_FB_DUMP" ] && printf '%s\n' 'set RECOMP_FB_DUMP_FLIP=1'
    # The flip capture fires on pgraph_d3d11_take_frame(), and that flag only
    # exists once pgraph_d3d11_init() has run -- which on Windows happens only
    # inside the D3D8 bring-up block. Without this a CPU-rasteriser capture
    # silently yields no flipNNN.bmp at all, while macOS, where the block is
    # unconditional, yields twenty-four. That asymmetry reads as "Windows never
    # presents a frame", and it is not about the renderer.
    [ -n "$ORACLE_FB_DUMP" ] && [ -z "$ORACLE_D3D11" ] && \
        printf '%s\n' 'set RECOMP_D3D8_PROBE=1'
    [ -n "$ORACLE_FLIP_TRACE" ] && \
        printf 'set RECOMP_FLIP_TRACE=%s\n' "$ORACLE_FLIP_TRACE"
    [ -n "$ORACLE_CONTIG_VERIFY" ] && printf '%s\n' 'set RECOMP_CONTIG_VERIFY=1'
    [ -n "$ORACLE_SYNC_EACH" ] && printf '%s\n' 'set RECOMP_D3D11_SYNC_EACH=1'
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
