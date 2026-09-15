#!/bin/sh
# A/B one RECOMP_* switch at a scene-verified mission, and say whether the arms
# separate.
#
# WHY THIS FILE EXISTS AT ALL. Four A/Bs were taken on 15 Sep 2026 -- selflink,
# se_while_trapped, trap_coalesce, metal_batch -- and every one of them was a
# throwaway shell loop written at the prompt. Their output directories survive
# under measure/; the scripts do not. So the metal_batch A/B, which was still
# running when that session ended, could not be resumed by the next one: the
# arms, the run length, the pad file and the scoring were all gone with the
# shell. Two of the four were also re-run from scratch after the first attempt
# was invalidated by a mistake this file now makes impossible (see PINNED
# BINARY below).
#
# An A/B harness is not the incidental part of a measurement. It is the part
# that has to be identical between arms, and the part the next person has to be
# able to repeat. It belongs in the tree.
#
# Usage:
#   ab_switch.sh <name> <RECOMP_VAR> [trials] [seconds] [pad-file]
#
#   ab_switch.sh batch RECOMP_METAL_BATCH 2 280
#   ab_switch.sh se    RECOMP_APU_SE_WHILE_TRAPPED 2 240
#
# Arm 0 is VAR=0, arm 1 is VAR=1. Everything else is held: same binary, same
# pad, same length, same staged HDD source, one run at a time.
#
# WHAT IT LAUNCHES. play_scripted.sh, unmodified, once per run. Not a private
# copy of its environment -- there is exactly one definition in this tree of
# how a scripted run is started, and an A/B that quietly diverged from it would
# be measuring the divergence. Everything this script wants is passed the way
# that script's own header documents: the switch and RECOMP_SEQ_REPORT in the
# environment, JSRF_BIN for the binary.
#
# WAITING FOR THIS SCRIPT TO FINISH: watch THIS SCRIPT, not the emulator.
#
#     while pgrep -f "ab_switch.sh <name>" >/dev/null; do sleep 60; done
#
# `pgrep -x jsrf_first_fault` is the obvious check and it is wrong: it is false
# between runs, while the HDD for the next one is being staged. Waiting on it
# reports "finished" several times during a normal A/B. On 15 Sep that led to a
# rebuild landing underneath a live A/B -- which the binary pin below caught and
# aborted, correctly -- and to a second A/B being launched on top of the first,
# whose runs then refused with "jsrf_first_fault is already running" and whose
# one completed run was contending for the GPU with the other A/B and measured
# the contention.
#
# PINNED BINARY. The binary is hashed before the first run and re-hashed before
# every run after it, and a change aborts the whole A/B. On 15 Sep a rebuild
# landed underneath a running A/B and silently changed a compiled-in default
# halfway through the arms; the arms were then not comparable and the result was
# discarded. Nothing about that was visible in the logs. It is visible now.
#
# ARM ORDER IS ALTERNATED, ABBA per pair of trials, so a host that drifts --
# thermals, a cache that fills, another process starting -- does not drift in
# step with one arm. The previous loops ran A then B every trial, which is the
# one order that confounds drift with the switch.
#
# HOW A RUN IS SCORED, and the two rules that come from this project's own
# retractions:
#
#   1. SCENE FIRST. A run that did not reach a mission is not compared with one
#      that did; it is reported and excluded. The marker is
#      CActSequence::m_dwNextMethod via RECOMP_SEQ_REPORT (28-35 = a mission or
#      the tutorial). NtOpenFile is NOT a scene marker -- it reads 131 in the VS
#      menu and 131 in gameplay -- and anything scored on it before 15 Sep 2026
#      was scored on the disc cache build.
#
#   2. FRAME TIME IS READ IN MISSION TIME, NOT WALL TIME. Both arms run the same
#      pad schedule, so the workload is comparable only once both are inside the
#      mission, and the mission does not start at the same second in two runs --
#      the logos vary by ten seconds or more. Every [FRAME-WIN] is therefore
#      tagged with the held= of the [JSRF-SEQ] line that closes its report
#      block, and the summary averages only windows at least MISSION_WARMUP
#      seconds into state 28-35. Before that is the mission's loading screen and
#      opening cutscene, which render nothing like play and are long enough to
#      swamp a 30 s window.
#
# WHAT IT CANNOT TELL YOU. Whether a switch is SAFE. RECOMP_METAL_BATCH is off
# because one interactive session hung on the SEGA screen and twelve scripted
# boots did not reproduce it; four more scripted boots cannot settle that, and
# this script says so in its own output rather than letting a green column read
# as a safety result.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: ab_switch.sh <name> <RECOMP_VAR> [trials] [seconds] [pad]}"
VAR="${2:?usage: ab_switch.sh <name> <RECOMP_VAR> [trials] [seconds] [pad]}"
TRIALS="${3:-2}"
LIMIT="${4:-280}"
PAD="${5:-$ROOT/diagnostics/jsrf_first_fault/pad/gameplay_nobarrage.pad}"
# Seconds into state 28-35 before a frame window counts. The mission's loading
# screen and opening cutscene sit inside state 30 -- the marker cannot separate
# them -- and they are the reason this is not zero.
WARMUP="${MISSION_WARMUP:-20}"

BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$ROOT/build-macos/jsrf-first-fault/measure/${NAME}_ab_${STAMP}"

[ -f "$PAD" ] || { echo "no pad file at $PAD" >&2; exit 1; }
[ -x "$BIN" ] || { echo "no binary at $BIN" >&2; exit 1; }

# The pin. openssl rather than shasum for the same reason run_common.sh gives:
# it is five times faster, and a check people wait 30 s for is a check they
# start skipping.
bin_hash() { openssl dgst -sha256 -r < "$BIN" | cut -d' ' -f1; }
PIN=$(bin_hash)

mkdir -p "$OUT" || exit 1
RESULTS="$OUT/RESULTS.txt"
: > "$RESULTS"

{
    echo "switch:  $VAR   (arm 0 = 0, arm 1 = 1)"
    echo "binary:  $BIN"
    echo "sha256:  $PIN"
    echo "pad:     $PAD"
    echo "length:  ${LIMIT}s x $TRIALS trials x 2 arms"
    echo "warmup:  ${WARMUP}s into the mission before a frame window counts"
    echo
} | tee "$OUT/CONFIG.txt"

# One run. $1 = tag, $2 = arm value. Leaves the log at $OUT/$1/stderr.log.
run_one() {
    tag="$1"; val="$2"
    if [ "$(bin_hash)" != "$PIN" ]; then
        echo "ABORTING at $tag: the binary changed since this A/B started." >&2
        echo "  Every run before this one was a different build. Start again." >&2
        exit 4
    fi
    mkdir -p "$OUT/$tag"
    echo "--- $tag: $VAR=$val ---"
    # play_scripted.sh writes into render-investigation/<name> and clears it
    # first, so each run gets its own name there and the log is copied here.
    env "$VAR=$val" RECOMP_SEQ_REPORT=1 JSRF_BIN="$BIN" \
        sh "$ROOT/diagnostics/jsrf_first_fault/play_scripted.sh" \
           "ab-$NAME-$tag" "@$PAD" "$LIMIT" > "$OUT/$tag/verdict.txt" 2>&1
    cp "$ROOT/build-macos/jsrf-first-fault/render-investigation/ab-$NAME-$tag/stderr.log" \
       "$OUT/$tag/stderr.log" 2>/dev/null
    score_one "$tag" "$val" >> "$RESULTS"
    tail -1 "$RESULTS"
}

# Read one run's log and print its line. Everything here is a grep over the
# log, so the scoring can be re-run against an archived directory later --
# which is exactly what could not be done to the abandoned metal_batch A/B.
score_one() {
    tag="$1"; val="$2"
    log="$OUT/$tag/stderr.log"
    [ -f "$log" ] || { echo "$tag $VAR=$val NO LOG -- the run did not start"; return; }
    /usr/bin/python3 "$ROOT/diagnostics/jsrf_first_fault/ab_score.py" \
        --tag "$tag" --var "$VAR" --value "$val" --warmup "$WARMUP" \
        --pad "$PAD" "$log"
}

t=1
while [ "$t" -le "$TRIALS" ]; do
    # ABBA: odd trials 0 then 1, even trials 1 then 0.
    if [ $((t % 2)) -eq 1 ]; then first=0; second=1; else first=1; second=0; fi
    run_one "t${t}_${NAME}${first}" "$first"
    run_one "t${t}_${NAME}${second}" "$second"
    t=$((t + 1))
done

echo
echo "=== $VAR: do the arms separate? ==="
/usr/bin/python3 "$ROOT/diagnostics/jsrf_first_fault/ab_score.py" \
    --summarise --var "$VAR" --warmup "$WARMUP" "$RESULTS" | tee -a "$RESULTS"
echo
echo "runs and logs: $OUT"
