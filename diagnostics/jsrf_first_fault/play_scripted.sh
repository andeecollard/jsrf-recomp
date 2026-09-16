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
#
# THE SCENE VERDICT IS OPT-IN. Gate 1 reads CActSequence::m_dwNextMethod,
# which needs RECOMP_SEQ_REPORT=1 in the environment; the run inherits it.
# Without it the script says SCENE: NOT MEASURED, because the counter it
# used to use -- NtOpenFile -- reads 131 in the VS menu and 131 in
# gameplay. See the long note at gate 1.
#
#   RECOMP_SEQ_REPORT=1 play_scripted.sh <name> @<pad> <secs>
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: play_scripted.sh <outname> <schedule|@file> [seconds]}"
SCHED="${2:?usage: play_scripted.sh <outname> <schedule|@file> [seconds]}"
LIMIT="${3:-240}"

# THE SCENE INSTRUMENT IS NOW ON BY DEFAULT, and the reason is a measurement.
#
# It used to be opt-in because "this title punishes instrumentation weight".
# That is true of the heavy probes (RECOMP_PAD_TRACE / SEQ_TRACE /
# FUNC_HIT_TRACE, see the header) but it was never measured for the 100 Hz
# CActSequence sampler alone. It has been now, over the 149 full-length runs of
# gameplay_nobarrage.pad under build-macos/jsrf-first-fault/render-investigation:
#
#     RECOMP_SEQ_REPORT=1   n=62   84% reached a mission
#     RECOMP_SEQ_REPORT off n=87   80% reached a mission
#
# The sampler costs nothing detectable. Leaving it off, on the other hand, cost
# 57 of the 63 runs taken on 16 Sep 2026 their only scene gate -- six of those
# runs never left the attract loop and were scored anyway.
#
# Set RECOMP_SEQ_REPORT=0 to suppress it; anything else (including unset) is on.
case "${RECOMP_SEQ_REPORT-}" in
    0) ;;
    *) RECOMP_SEQ_REPORT=1; export RECOMP_SEQ_REPORT ;;
esac

# Seconds after which a run that has not reached a mission is killed.
#
# MEASURED, same 149-run corpus, and both bounds have real margin:
#   * every crash landed between t=24.0 s and t=43.0 s. Not one after 43 s.
#   * the latest any run entered state 28-35 was t=44.7 s; the slowest to show
#     it in the log did so by t=51 s.
# So by t=60 s the outcome is already decided, and a doomed 280 s run becomes a
# doomed 60 s one. Set ABORT_AT=0 to disable.
ABORT_AT="${ABORT_AT:-60}"
OUT="$ROOT/build-macos/jsrf-first-fault/render-investigation/$NAME"
SCRATCH="${PLAY_SCRATCH:-/tmp/jsrf-playscripted-$NAME}"
BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"

if [ ! -x "$BIN" ]; then
    echo "no binary at $BIN" >&2
    exit 1
fi
# Same staleness guard as play.sh. *_test.c AND *_test.m are excluded: those
# are separate CMake targets that are NOT linked into jsrf_first_fault, so
# touching one made this refuse to run a binary that was in fact current. A
# guard that fires when nothing is wrong is one people start passing
# JSRF_ALLOW_STALE to, which defeats it entirely. A measurement taken against
# yesterday's binary is worse than no measurement, because it looks like one.
#
# The .m half was missing until 16 Sep 2026 and cost a two-arm bisect: adding
# vsh_msl_diff_test.m -- a ctest target, never linked into the game -- refused
# every run until the pattern was widened. The comment above already said what
# the rule was; the pattern just did not implement it.
NEWER=$(find "$ROOT/src" "$ROOT/diagnostics/jsrf_first_fault" -name '*.c' -o -name '*.h' -o -name '*.m' 2>/dev/null \
        | grep -v ' 2\.c$' | grep -vE '_test\.(c|m)$' \
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

# The early abort, and it deliberately reads the LOG rather than the guest.
#
# Nothing here runs inside the process being measured: it is a shell loop over
# a file the run is already writing. That is the whole design constraint --
# heavy probes provoke the input-poll stall, so the detector for a bad run must
# not be able to cause one.
#
# The test is the scene marker and nothing else. The NtOpenFile count is NOT
# used even though it separates these runs perfectly today (1408+ = mission,
# 1341 = attract, 0 errors over the 65 runs that also carry the scene marker):
# it reads 131 in EVERY scene once the staged HDD carries a complete
# Media/Cache, so a threshold on it would abort every run in that regime. See
# the long note at gate 1.
EARLY=""
if [ "$ABORT_AT" != "0" ] && [ "${RECOMP_SEQ_REPORT-}" = "1" ]; then
    ( sleep "$ABORT_AT"
      kill -0 $PID 2>/dev/null || exit 0
      now=$(grep '\[JSRF-SEQ\] now=' "$OUT/stderr.log" 2>/dev/null \
            | tail -1 | sed -n 's/.*now=\([0-9][0-9]*\).*/\1/p')
      # No reading at all is NOT a reason to abort. The marker rides the
      # periodic report, so a raised RECOMP_REPORT_MS -- or a sampler that
      # failed -- leaves it empty, and killing the run on that would be
      # guessing. Let it finish; gate 1 already says "INSTRUMENT FAILED".
      case "${now:-none}" in
          none)                    exit 0 ;;
          28|29|30|31|32|33|34|35) exit 0 ;;
      esac
      echo "state=${now:-none}" > "$OUT/ABORTED_EARLY"
      kill -TERM $PID 2>/dev/null; sleep 5; kill -9 $PID 2>/dev/null ) &
    EARLY=$!
fi

wait $PID
kill $WATCH 2>/dev/null
[ -n "$EARLY" ] && kill $EARLY 2>/dev/null
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
# GATE 1 USED TO BE THE NtOpenFile COUNT. IT IS NOT A SCENE MARKER. It said:
#   ~1342 opens / 260 distinct files / live=133  = TITLE plateau
#    1408 opens / 264 distinct files / live=56   = New Game reached
# (CLAUDE_HANDOVER_2026-09-12_THE_IDLE_LOOP_FREEZE.txt:102,209 and
# CLAUDE_HANDOVER_2026-09-04_STALL_ELIMINATION.txt:151.)
#
# Measured 15 Sep 2026, three runs, same binary, same gen:
#   .../play/20260915-122828-human-gameplay  screenshot: GAMEPLAY   131 opens
#   .../play/20260915-120746-topwrite        screenshot: VS MENU    131 opens
#   .../play/20260915-121615-relink          screenshot: VS MENU    131 opens
# Not merely the same count -- the same 22 distinct paths with the same
# multiplicities, byte for byte, and the last open in all three lands at
# t=79 s, long before either scene is reached.
#
# WHAT THE COUNTER WAS ACTUALLY MEASURING: the cache build. The 1410-open run
# (.../play/20260915-110630) opened Cache00.tbl~ .. DmCache09.tbl~ and took 253
# FAILED opens probing for them; its HDD's Media/Cache is dated 11:11, the
# minute that run ended. Once the staged emulated-hdd carries a complete
# Media/Cache and JSRF_CACHE_COMPLETE.CMP -- which every tree here now does --
# the title skips StartBuildCache and the count collapses to 131 in EVERY
# scene. The old gate measured whether the disc cache had to be rebuilt.
#
# Frame rate does not separate them either: the VS menu's 3D stage preview ran
# 2,706 draws/s against gameplay's 3,042.
#
# So gate 1 now reads the title's OWN top-level state,
# CActSequence::m_dwNextMethod, via RECOMP_SEQ_REPORT (diagnostics/.../main.c).
# It is opt-in because it starts a 100 Hz sampler thread and this title
# punishes instrumentation weight; the run below inherits it from the
# environment, so:
#
#     RECOMP_SEQ_REPORT=1 play_scripted.sh <name> @<pad> <secs>
#
# With it off there is no scene verdict, and the script says so rather than
# guessing from a counter that cannot see the difference.
echo
echo "=== did it reach gameplay, and did it play? ==="
if [ -f "$OUT/ABORTED_EARLY" ]; then
    echo "  ABORTED at ${ABORT_AT}s: no mission by then, so there was not going"
    echo "           to be one ($(cat "$OUT/ABORTED_EARLY")). The counters below"
    echo "           are from a ${ABORT_AT}s run and are NOT comparable with a"
    echo "           full-length one. Re-run; do not score this."
fi
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

# Gate 0c: did the guest's SOUND SERVER stop before the title handed over?
#
# THIS IS WHAT DECIDES A STALLED RUN, and it is not the pad.
#
# The title's level-5 sound worker (guest body 0x0013B2A0, tick counter at
# 0x0025EFB0) is already watched, unconditionally, by jsrf_adx_watch in
# main.c -- it prints "[ADX] tick=N" on every change and "[ADX] tick STUCK at
# N for Ds" once the value has been still for ten seconds. No new probe is
# needed and none is added here.
#
# MEASURED over the 149 full-length gameplay_nobarrage runs under
# build-macos/jsrf-first-fault/render-investigation, classified by scene:
#
#     the worker never froze        104 reached a mission,  1 did not
#     it froze at t >= 25 s          12 reached a mission,  0 did not
#     it froze at t <  25 s           0 reached a mission,  6 did not
#
# The earliest any run has handed the title over to the menu is t=25.0 s. If
# the worker dies before that, WaitEndTitle never completes and no number of A
# presses helps: both stalled runs examined in detail (e32_2, idxcap) polled
# the pad at ~176/s -- a healthy rate -- through all eight scheduled presses.
#
# AND THE AUDIO DEVICE IS WHAT CHANGES HOW OFTEN IT FREEZES. Same corpus,
# non-crashed runs of 100 s or more:
#
#     device live (output ready)      2 of 103 froze    ( 2%)
#     device forced off               13 of 14 froze    (93%)
#     device failed (CoreAudio)        3 of 6  froze    (50%)
#
# which is the OPPOSITE of what this file's header used to claim from n=5.
# Forcing the device off does not make the boot reliable; it makes the sound
# worker freeze in nearly every run, and the runs it ruins are the ones where
# the freeze lands early. Net, by scene: 1.6% of device-live runs stalled
# against 21% of forced-off runs. Forced-off still has its own advantage --
# every one of the 20 crashes in the corpus had audio live or failed, none had
# it off -- so this is a trade between two failures, not a setting to prefer.
ADX_STUCK=$(grep '\[ADX\] tick STUCK' "$OUT/stderr.log" 2>/dev/null | tail -1)
if [ -n "$ADX_STUCK" ]; then
    ADX_FOR=$(printf '%s' "$ADX_STUCK" | sed -n 's/.*for \([0-9]*\)s.*/\1/p')
    ADX_LAST=$(grep '\[FB\] t=' "$OUT/stderr.log" | tail -1 \
               | sed 's/.*t= *\([0-9.]*\).*/\1/')
    ADX_AT=$(/usr/bin/python3 -c "print('%.0f' % ($ADX_LAST - $ADX_FOR))" 2>/dev/null)
    if [ -n "$ADX_AT" ] && [ "$ADX_AT" -lt 25 ] 2>/dev/null; then
        echo "  SOUND:   WORKER FROZE AT t=${ADX_AT}s, BEFORE THE TITLE HANDS OVER."
        echo "           This is the stall. Every run in the corpus that froze"
        echo "           before t=25 s stayed in the attract loop (6 of 6), and"
        echo "           every run that froze later reached a mission (12 of 12)."
        echo "           Nothing about the pad schedule is at fault; re-run."
    else
        echo "  SOUND:   worker froze at t=${ADX_AT:-?}s (after the handover; benign)."
    fi
else
    echo "  SOUND:   worker ran to the end of the run (no [ADX] tick STUCK)."
fi

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

# Gate 1: scene, from CActSequence::m_dwNextMethod. See the long note above for
# why this is no longer the NtOpenFile count.
#
# The index groups, all read straight off the dispatch table at 0x0020D2B8 and
# the `mov [esi+0x48], N` that ends each method:
#    10-13  title screen            28-31  a story-or-VS mission exists
#    14-23  title menus             32-35  the tutorial
#    56-59  the VS menu chain       44-47  the graffiti menu
#
# 28-35 is the honest boundary for "reached play": state 30 means a mission
# ACTION OBJECT exists, which covers the mission's own loading screen, its
# opening cutscene and its pause menu as well as skating. It separates the menu
# shell from a mission. It does not separate playing from being paused inside
# one -- use gate 2 for that.
SEQ_LINE=$(grep '\[JSRF-SEQ\] now=' "$OUT/stderr.log" | tail -1)
SEQ_IDX=$(printf '%s' "$SEQ_LINE" | sed -n 's/.*now=\([0-9][0-9]*\).*/\1/p')
echo "  NtOpenFile:       $OPENS   (boot/cache-build only -- NOT a scene marker)"
if [ -n "$SEQ_IDX" ]; then
    printf '%s\n' "$SEQ_LINE" | sed 's/^ */  /'
    case "$SEQ_IDX" in
        28|29|30|31|32|33|34|35) SCENE_OK=1 ;;
        *)                       SCENE_OK=0 ;;
    esac
    if [ "$SCENE_OK" -eq 1 ]; then
        echo "  SCENE:   in a mission or the tutorial (state $SEQ_IDX)."
    else
        echo "  SCENE:   NEVER REACHED A MISSION -- ended in state $SEQ_IDX."
        echo "           The boot prefix did not land, or the schedule stopped in"
        echo "           a menu. Logo length varies by ten seconds or more between"
        echo "           runs. Nothing about gameplay, audio uptime or voice"
        echo "           retirement can be concluded from this run; re-run it."
        echo "           Read the dwell: list on the line above for where it sat."
    fi
elif grep -q '\[JSRF-SEQ\] NO READING' "$OUT/stderr.log"; then
    grep '\[JSRF-SEQ\] NO READING' "$OUT/stderr.log" | tail -1 | sed 's/^ */  /'
    echo "  SCENE:   INSTRUMENT FAILED, not measured. The sampler ran and could"
    echo "           not read CActSequence. Do not score this run."
    SCENE_OK=0
else
    echo "  SCENE:   NOT MEASURED. Re-run with RECOMP_SEQ_REPORT=1 in the"
    echo "           environment; the NtOpenFile count above cannot tell a menu"
    echo "           from gameplay (see the note in this script)."
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
