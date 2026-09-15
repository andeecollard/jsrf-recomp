#!/bin/sh
# An unattended run that actually PLAYS, and reports whether it did.
#
# This sits between the two scripts that already exist, because neither can
# measure gameplay:
#
#   play.sh          light enough to measure, but has no schedule -- it needs a
#                    person holding the controller, so every number past the
#                    title screen has cost a human playthrough
#   run_scripted.sh  has a schedule, but turns on RECOMP_PAD_TRACE, SEQ_TRACE,
#                    FUNC_HIT_TRACE and SCENE_REPORT together. Heavy probes
#                    destabilise this title: they provoke the input-poll stall,
#                    which then reads as a dead controller. A gameplay
#                    measurement taken through it is measuring the probes.
#
# So: run_scripted.sh's schedule with play.sh's environment. Nothing is traced
# unless you ask for it, and you ask by putting the switch in the environment,
# which is also what makes this an A/B harness:
#
#   play_scripted.sh se-off  @diagnostics/jsrf_first_fault/pad/gameplay.pad 240
#   RECOMP_APU_SE_WHILE_TRAPPED=1 \
#     play_scripted.sh se-on @diagnostics/jsrf_first_fault/pad/gameplay.pad 240
#
# That pair is the specific thing this script was written for.
# RECOMP_APU_SE_WHILE_TRAPPED has been built and off since edbd87d and has
# never been evaluated, because evaluating it needed someone to grind a rail.
#
# THE LIVE AUDIO DEVICE DECIDES WHETHER THE SCRIPTED BOOT WORKS.
#
# Measured 14 Sep 2026, same binary, same schedule, same boot prefix, four runs:
#
#     live audio device      1342, 1342 opens         0 of 2 left the title
#     device forced off      1408, 1410, 1342 opens   2 of 3 reached New Game
#
# CORRECTED: this said "clean separation" on the first two runs of the
# forced-off arm. The third forced-off run stayed at 1342, so the effect is
# 2/3 against 0/2, not 2/2 against 0/2. It is a lean, not a switch, and it is
# n=5 in total. (That third run ALSO took a 76 s input-poll stall, so it is
# unhealthy on a second axis -- see the INPUT verdict below.) A mechanism does
# fit: the APU throttles to real
# time against the device (slept= is essentially the whole run), so with no
# device the title advances through the logos at a different rate and the fixed
# pad schedule lands differently. Two runs per arm is not proof, but it is a
# controlled result rather than a hunch, and it is reproducible on demand:
#
#     SDL_AUDIODRIVER=no_such_driver play_scripted.sh <name> @<pad> <secs>
#
# makes apu_sdl2_init fail deterministically, which is exactly the state the
# successful runs were in.
#
# THE TENSION IS REAL AND THERE IS NO TRICK FOR IT. Audio measurements need the
# device ON, and that is the arm in which the boot does not reach gameplay. So
# the trap-storm A/B (RECOMP_APU_SE_WHILE_TRAPPED) cannot currently be taken
# unattended: the run that reaches gameplay has no audio, and the run with
# audio does not reach gameplay. Either the boot schedule has to become robust
# with audio live, or that experiment needs a person on the controller.
#
# Use SDL_AUDIODRIVER=no_such_driver for anything NOT about audio -- scene,
# input, frame rate, object state, guest CPU profiling -- where it turns a
# one-in-three boot into a reliable one.
#
# Usage:  play_scripted.sh <outname> <schedule|@file> [seconds]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: play_scripted.sh <outname> <schedule|@file> [seconds]}"
SCHED="${2:?usage: play_scripted.sh <outname> <schedule|@file> [seconds]}"
LIMIT="${3:-240}"
OUT="$ROOT/build-macos/jsrf-first-fault/render-investigation/$NAME"
SCRATCH="${PLAY_SCRATCH:-/tmp/jsrf-playscripted-$NAME}"
BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"

if [ ! -x "$BIN" ]; then
    echo "no binary at $BIN" >&2
    exit 1
fi
# Same staleness guard as play.sh. *_test.c is excluded: those are separate
# CMake targets that are NOT linked into jsrf_first_fault, so touching one
# made this refuse to run a binary that was in fact current. A guard that
# fires when nothing is wrong is one people start passing JSRF_ALLOW_STALE
# to, which defeats it entirely. A measurement taken against yesterday's
# binary is worse than no measurement, because it looks like one.
NEWER=$(find "$ROOT/src" "$ROOT/diagnostics/jsrf_first_fault" -name '*.c' -o -name '*.h' -o -name '*.m' 2>/dev/null \
        | grep -v ' 2\.c$' | grep -vE '_test\.c$' \
        | while read -r f; do [ "$f" -nt "$BIN" ] && echo "$f"; done | head -3)
if [ -n "$NEWER" ]; then
    echo "WARNING: $BIN is older than these sources -- rebuild, or you are measuring the previous build:" >&2
    echo "$NEWER" | sed 's/^/    /' >&2
    [ -n "${JSRF_ALLOW_STALE:-}" ] || { echo "  (set JSRF_ALLOW_STALE=1 to run anyway)" >&2; exit 1; }
fi
if pgrep -x jsrf_first_fault >/dev/null 2>&1; then
    echo "REFUSING: jsrf_first_fault is already running (pid $(pgrep -x jsrf_first_fault | tr '\n' ' '))" >&2
    exit 2
fi

STOCK="${JSRF_HDD_SRC:-$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd}"
[ -d "$STOCK" ] || {
    echo "no emulated-hdd at $STOCK -- set JSRF_HDD_SRC" >&2; exit 1; }
GAME_DIR="${JSRF_GAME_DIR:-$ROOT/../Jet Set Radio Future (US)}"
[ -f "$GAME_DIR/default.xbe" ] || {
    echo "no default.xbe under $GAME_DIR -- set JSRF_GAME_DIR" >&2; exit 1; }

rm -rf "$OUT" "$SCRATCH"; mkdir -p "$OUT" "$SCRATCH"
cp -R "$STOCK" "$SCRATCH/hdd"

echo "schedule: $SCHED"
echo "binary:   $BIN"
echo "log:      $OUT/stderr.log   (${LIMIT}s)"
cd "$ROOT" || exit 1
RECOMP_XBE_PATH="$GAME_DIR/default.xbe" RECOMP_GAME_DIR="$GAME_DIR" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_PAD_INJECT=1 \
RECOMP_PAD_SCRIPT="$SCHED" \
RECOMP_REPORT_MS="${REPORT_MS:-10000}" \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$BIN" > "$OUT/stderr.log" 2>&1 &
PID=$!
( sleep "$LIMIT"; kill -TERM $PID 2>/dev/null; sleep 5; kill -9 $PID 2>/dev/null ) &
WATCH=$!
wait $PID
kill $WATCH 2>/dev/null
rm -rf "$SCRATCH"

# The whole point of the script: say what happened, in the two terms that can
# actually go wrong, and refuse to conflate them.
#
# These are SEPARATE questions and the first version of this script ran them
# together. It reported "this run PLAYED" for a run that never left the attract
# screen, on the strength of off=1 -- one voice retired somewhere in the
# title's own audio. Reaching gameplay and playing once there are different
# failures with different fixes, so they get different lines.
#
# Gate 1 costs nothing extra. [FILE] lines are on by default, and the
# NtOpenFile count is a reliable, long-established scene marker:
#   ~1342 opens / 260 distinct files / live=133  = TITLE plateau
#    1408 opens / 264 distinct files / live=56   = New Game reached
# (CLAUDE_HANDOVER_2026-09-12_THE_IDLE_LOOP_FREEZE.txt:102,209 and
# CLAUDE_HANDOVER_2026-09-04_STALL_ELIMINATION.txt:151.) It needs none of the
# heavy probes, which is what makes it usable from a run light enough to be
# worth measuring.
echo
echo "=== did it reach gameplay, and did it play? ==="
LAST_VOICE=$(grep '\[APU-VOICE\]' "$OUT/stderr.log" | tail -1)
LAST_FRAME=$(grep '\[APU-FRAME\]' "$OUT/stderr.log" | tail -1)
OPENS=$(grep -c 'NtOpenFile' "$OUT/stderr.log")
FIRED=$(grep -c '\[PAD-SCRIPT\] .* fire ' "$OUT/stderr.log")
echo "${LAST_VOICE:-  [APU-VOICE] (no report -- did the run reach one?)}"
echo "${LAST_FRAME:-  [APU-FRAME] (no report)}"
echo "  pad events fired: $FIRED"

# Gate 0b: did the guest stop READING the pad partway through?
#
# The input-poll stall is a known failure mode here and it is invisible in
# every other number: the schedule still runs to completion, the last event
# still fires at its scheduled time, and the average poll rate barely moves,
# because the stall is one long silence inside an otherwise healthy run.
# Measured 14 Sep 2026: a run that fired 128 of 202 events had a SEVENTY-SIX
# SECOND gap between t=164 and t=240, with a 19 ms mean poll interval either
# side -- identical, to three significant figures, to a run that fired all 202.
# Counting events or averaging polls cannot see it. The gap can.
#
# Anything scheduled inside the gap simply did not happen, so a run with a
# large one cannot support a claim about what the input did or did not cause.
BIGGEST_GAP=$(grep '\[PAD-SCRIPT\] .* fire ' "$OUT/stderr.log" \
    | sed 's/.*t= *\([0-9.]*\).*/\1/' \
    | awk 'NR>1 && $1-p>g { g=$1-p; a=p; b=$1 } { p=$1 } END { printf "%.1f %.2f %.2f", g, a, b }')
GAP_S=${BIGGEST_GAP%% *}
case "$GAP_S" in
    ''|*[!0-9.]*) GAP_S=0 ;;
esac
# 10 s: comfortably above this schedule's own 9 s spacing at the boot/play seam,
# so ordinary gaps do not trip it.
if [ "$(/usr/bin/python3 -c "print(1 if $GAP_S >= 10 else 0)" 2>/dev/null || echo 0)" = "1" ]; then
    echo "  INPUT:   POLL STALL -- ${BIGGEST_GAP%% *}s with no event firing" \
         "(t=$(echo "$BIGGEST_GAP" | cut -d' ' -f2) to $(echo "$BIGGEST_GAP" | cut -d' ' -f3))."
    echo "           The guest stopped reading the pad. Everything scheduled in"
    echo "           that window did not happen; do not attribute anything to input."
else
    echo "  INPUT:   no poll stall (largest gap ${GAP_S}s)"
fi
echo "  NtOpenFile:       $OPENS   (~1342 = title plateau, 1408 = New Game)"

# Gate 0, and it comes first because it invalidates everything below it.
#
# If CoreAudio refuses the device, apu_sdl2_init() fails, the APU falls back to
# a waveOut path that does nothing on this host, and [APU-PACE] reads
# gen_hz=0 batches=0 frames=0 -- while the sound engine still reports hundreds
# of thousands of rendered subframes, so `se=` looks perfectly healthy. Every
# audio number in the run is then meaningless.
#
# This cost two wrong conclusions in one session: first "the new binary is an
# audio regression" (it was not), then "the KeSynchronizeExecution bridge
# silenced gameplay" (it did not). The actual line in the log was
#
#     [APU-SDL] output open failed: CoreAudio error (AudioQueueStart): -66681
#
# which is a HOST failure and nothing to do with the build. Proved by
# reproducing it in a standalone 12-line SDL2 program that links none of this
# code: same error, same host, same minute. -66681 is kAudioQueueErr_CannotStart.
#
# It is NOT always transient -- it survived killing every jsrf process and did
# not clear on its own. The fix is to restart the audio daemon:
#
#     sudo killall coreaudiod
#
# (system-wide: it briefly interrupts audio in every other app, so do not do it
# under someone's video call.) Check this line BEFORE reading any audio counter,
# which is why it prints first among the verdicts.
if grep -q '\[APU-SDL\] output open failed' "$OUT/stderr.log"; then
    echo "  AUDIO:   DEVICE NEVER OPENED --" \
         "$(grep -m1 '\[APU-SDL\] output open failed' "$OUT/stderr.log" | sed 's/^ *//')"
    echo "           Every audio figure in this run is void, gen_hz=0 included."
    echo "           This is a host CoreAudio failure, not a code change. Re-run."
elif grep -q '\[APU-SDL\] output ready' "$OUT/stderr.log"; then
    echo "  AUDIO:   device open --" \
         "$(grep -m1 '\[APU-SDL\] output ready' "$OUT/stderr.log" | sed 's/^ *\[APU-SDL\] //')"
elif grep -q '\[APU-SDL\] SDL audio initialization failed' "$OUT/stderr.log"; then
    # Deliberate, if SDL_AUDIODRIVER was set to something that does not exist.
    # That is the supported way to get a reliable scripted boot -- see below.
    echo "  AUDIO:   OFF BY REQUEST --" \
         "$(grep -m1 '\[APU-SDL\] SDL audio initialization failed' "$OUT/stderr.log" | sed 's/^ *//')"
    echo "           Audio counters are void; scene and input figures are fine."
else
    echo "  AUDIO:   no [APU-SDL] line at all -- which backend did it pick?"
    grep -m1 -E '\[APU\] (Using|XAudio2)' "$OUT/stderr.log" | sed 's/^ *//;s/^/           /'
fi

# Gate 1: scene.
if [ "$OPENS" -ge 1400 ]; then
    echo "  SCENE:   reached New Game."
    SCENE_OK=1
else
    echo "  SCENE:   NEVER LEFT THE TITLE. The boot prefix did not land -- logo"
    echo "           length varies by ten seconds or more between runs. Nothing"
    echo "           about gameplay, audio uptime or voice retirement can be"
    echo "           concluded from this run; re-run it."
    SCENE_OK=0
fi

# Gate 2: did the player actually do anything once there.
OFF=$(printf '%s' "$LAST_VOICE" | sed -n 's/.* off=\([0-9]*\).*/\1/p')
if [ -z "$OFF" ]; then
    echo "  PLAYED:  no APU voice report in the log."
elif [ "$SCENE_OK" -eq 0 ]; then
    echo "  PLAYED:  not asked -- gate 1 failed. (off=$OFF is title audio.)"
elif [ "$OFF" -ge 40 ]; then
    echo "  PLAYED:  yes -- off=$OFF retired voices, the same order as a human"
    echo "           playthrough (off=103). This run is usable."
elif [ "$OFF" -gt 0 ]; then
    echo "  PLAYED:  MARGINAL. off=$OFF is above zero but far below the off=103"
    echo "           a person produces. The schedule reached gameplay and then"
    echo "           did little; treat conclusions about the trap storm as weak."
else
    echo "  PLAYED:  NO. off=0 -- the player was parked, which is the signature"
    echo "           this file exists to eliminate. Void for anything about"
    echo "           voice retirement, the trap storm, or audio uptime."
fi
