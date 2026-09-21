#!/bin/sh
# xemu_capture.sh -- capture an NV2A method/surface trace from xemu for a
# scene-matched differential against our own renderer.
#
# WHY THIS EXISTS. xemu runs the SAME guest XBE against a different GPU model,
# so its pgraph trace is ground truth for WHAT THE GUEST SUBMITTED. It says
# nothing whatever about our host code. Use it to answer "did the guest ask for
# this?" and never to answer "is our backend right?".
#
# SCOPE, AND WHY. A full nv2a_pgraph_method trace of a menu is hundreds of MB
# because every method carries its parameter on its own line. QEMU's trace
# machinery cannot filter by method NUMBER, only by event NAME, so the scoping
# here is by event set, by wall-clock and by file size -- all three, because
# only the third has a bound you can promise:
#
#   --preset surfaces  nv2a_pgraph_surface_* + flip_* + method_unhandled.
#                      Per-surface and per-flip, not per-method: a minute is
#                      a few hundred KB. Answers "what surfaces exist, which
#                      one is the target, how many flips, which methods xemu
#                      itself does not implement".
#   --preset methods   the above plus nv2a_pgraph_method_abbrev, which collapses
#                      a run of one repeated method to a single line with a
#                      count. Gives the METHOD INVENTORY and per-method counts.
#                      Carries NO parameter values.  (default)
#   --preset full      the above plus nv2a_pgraph_method: every method with its
#                      parameter. This is the only preset that can reconstruct
#                      combiner / output-control words. Hundreds of MB/min --
#                      keep --seconds small and trust --max-mb.
#
# THE MANIFEST IS NOT DECORATION. capture.json records the exact command, the
# event set, the wall-clock, the byte count and the trace's sha256. A signature
# belongs to the run it was measured in; without the manifest a trace file is
# an anonymous number.
#
# THIS OPENS A WINDOW AND, UNLESS --audio, IS SILENT (SDL_AUDIODRIVER=dummy,
# the same muting our own harness runs use so the two sides match).
#
# Nothing here is opt-out safe: it is a capture tool, it launches an emulator,
# and it kills it on a timer. --dry-run prints the command and exits.

set -eu

PRESET=methods
SECONDS_LIMIT=75
MAX_MB=768
OUT=
DRY=0
AUDIO=0

XEMU=${XEMU:-/Applications/xemu.app/Contents/MacOS/xemu}
BOOTROM=${XEMU_BOOTROM:-/Applications/mcpx_1.0.bin}
BIOS=${XEMU_BIOS:-/Applications/Complex_4627.bin}
HDD=${XEMU_HDD:-/Applications/xbox_hdd.qcow2}
ISO=${XEMU_ISO:-/Users/andrewcollard/Downloads/JSRF-US.xiso.iso}
EEPROM=${XEMU_EEPROM:-$HOME/Library/Application Support/xemu/xemu/eeprom.bin}

usage() {
    sed -n '2,45p' "$0"
    echo
    echo "usage: $0 [--preset surfaces|methods|full] [--seconds N] [--max-mb N]"
    echo "          [--out DIR] [--audio] [--dry-run]"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)   PRESET=$2; shift 2 ;;
        --seconds)  SECONDS_LIMIT=$2; shift 2 ;;
        --max-mb)   MAX_MB=$2; shift 2 ;;
        --out)      OUT=$2; shift 2 ;;
        --audio)    AUDIO=1; shift ;;
        --dry-run)  DRY=1; shift ;;
        -h|--help)  usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$OUT" ] || OUT="$HOME/jsrf-build/xemu-$(date +%Y-%m-%d_%H%M)-$PRESET"

# Refuse early and by name. A missing BIOS surfaces as a black window three
# minutes in, which is indistinguishable from the bug we are chasing.
for f in "$XEMU" "$BOOTROM" "$BIOS" "$HDD" "$ISO"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 2; }
done

mkdir -p "$OUT"
EVENTS="$OUT/events.txt"
TRACE="$OUT/trace.txt"
XLOG="$OUT/xemu-stdout.log"

{
    echo nv2a_pgraph_surface_create_color
    echo nv2a_pgraph_surface_create_zeta
    echo nv2a_pgraph_surface_hit_color
    echo nv2a_pgraph_surface_hit_zeta
    echo nv2a_pgraph_surface_match_color
    echo nv2a_pgraph_surface_match_zeta
    echo nv2a_pgraph_surface_target
    echo nv2a_pgraph_surface_download
    echo nv2a_pgraph_surface_upload
    echo nv2a_pgraph_surface_invalidated
    echo nv2a_pgraph_surface_evict_reason
    echo nv2a_pgraph_surface_evict_overlapping
    echo nv2a_pgraph_surface_render_to_texture
    echo nv2a_pgraph_flip_stall
    echo nv2a_pgraph_flip_increment_write
    echo nv2a_pgraph_method_unhandled
    case "$PRESET" in
        surfaces) ;;
        methods)  echo nv2a_pgraph_method_abbrev ;;
        full)     echo nv2a_pgraph_method_abbrev; echo nv2a_pgraph_method ;;
        *) echo "unknown preset: $PRESET" >&2; exit 2 ;;
    esac
} > "$EVENTS"

set -- \
    -machine "xbox,bootrom=$BOOTROM,kernel-irqchip=off,avpack=hdtv" \
    -device "smbus-storage,file=$EEPROM" \
    -bios "$BIOS" \
    -m 128 \
    -drive "index=0,media=disk,file=$HDD,locked=on" \
    -drive "index=1,media=cdrom,file=$ISO" \
    -display xemu \
    -device usb-hub,port=1,ports=4 \
    -gdb tcp:127.0.0.1:1234 \
    -trace "events=$EVENTS,file=$TRACE"

printf '%s' "$XEMU"; for a in "$@"; do printf " '%s'" "$a"; done; echo
if [ "$DRY" = 1 ]; then
    echo "(dry run -- nothing launched; event set is in $EVENTS)"
    exit 0
fi

START=$(date +%s)
if [ "$AUDIO" = 1 ]; then
    "$XEMU" "$@" > "$XLOG" 2>&1 &
else
    SDL_AUDIODRIVER=dummy "$XEMU" "$@" > "$XLOG" 2>&1 &
fi
PID=$!
echo "xemu pid $PID; trace -> $TRACE; limit ${SECONDS_LIMIT}s / ${MAX_MB}MB"

REASON=exited
while kill -0 "$PID" 2>/dev/null; do
    sleep 1
    NOW=$(date +%s)
    if [ $((NOW - START)) -ge "$SECONDS_LIMIT" ]; then REASON=time; break; fi
    if [ -f "$TRACE" ]; then
        MB=$(( $(wc -c < "$TRACE") / 1048576 ))
        if [ "$MB" -ge "$MAX_MB" ]; then REASON=size; break; fi
    fi
done
if kill -0 "$PID" 2>/dev/null; then kill "$PID" 2>/dev/null || true; sleep 2; fi
if kill -0 "$PID" 2>/dev/null; then kill -9 "$PID" 2>/dev/null || true; fi
END=$(date +%s)

BYTES=0; [ -f "$TRACE" ] && BYTES=$(wc -c < "$TRACE")
SHA=$(shasum -a 256 "$TRACE" 2>/dev/null | awk '{print $1}')
LINES=0; [ -f "$TRACE" ] && LINES=$(wc -l < "$TRACE")

{
    echo '{'
    echo "  \"captured_local\": \"$(date -Iseconds)\","
    echo "  \"preset\": \"$PRESET\","
    echo "  \"stopped_because\": \"$REASON\","
    echo "  \"wall_seconds\": $((END - START)),"
    echo "  \"audio\": $AUDIO,"
    echo "  \"trace_bytes\": $BYTES,"
    echo "  \"trace_lines\": $LINES,"
    echo "  \"trace_sha256\": \"$SHA\","
    echo "  \"iso\": \"$ISO\","
    echo "  \"bios\": \"$BIOS\","
    echo "  \"hdd\": \"$HDD\","
    printf '  "events": ['
    sed 's/.*/"&"/' "$EVENTS" | paste -sd, -
    echo '],'
    printf '  "argv": ['
    printf '"%s"' "$XEMU"; for a in "$@"; do printf ',"%s"' "$a"; done
    echo ']'
    echo '}'
} > "$OUT/capture.json"

echo "stopped: $REASON after $((END - START))s; $LINES lines, $BYTES bytes"
echo "manifest: $OUT/capture.json"
