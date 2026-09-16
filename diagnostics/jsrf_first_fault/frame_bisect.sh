#!/bin/sh
# Capture GAMEPLAY FRAMES from each arm of a renderer bisect, one arm at a
# time, so the arms can be compared as pictures.
#
# WHY THIS FILE EXISTS. The open item at the top of
# HANDOVER_2026-09-15_SIX_DEFECTS_FIXED_AND_THE_PICTURE_IS_STILL_WRONG.txt is
# "run the depth and stencil bisect arms to a result". Both arms had been
# started twice and killed twice, by the previous session's own runs competing
# for the binary, and an earlier attempt at the same pair was VOIDED because it
# was scored with a black-pixel metric that was counting letterboxing and intro
# art. Two separate ways to waste a run, and both are harness problems:
#
#   * nothing serialised the runs, so a second one started underneath the first
#   * nothing captured the pictures, so the arms were scored by a statistic
#
# This does both. It is ab_switch.sh's discipline -- serial runs, a pinned
# binary, arms that name themselves -- pointed at frames instead of frame time,
# because the question here is what the renderer DREW, not how fast.
#
# Usage:
#   frame_bisect.sh <name> <arm>[ <arm>...]
#
#   frame_bisect.sh dsbisect sw hw depth-always no-stencil
#
# Arms, each a name and the environment it adds:
#   sw            RECOMP_METAL_HW unset               the known-good reference
#   hw            RECOMP_METAL_HW=1                   the path under suspicion
#   depth-always  hw + RECOMP_METAL_HW_DEPTH_ALWAYS=1 depth test neutered
#   no-stencil    hw + RECOMP_METAL_HW_NO_STENCIL=1   stencil unit neutered
#   no-batch      hw + RECOMP_METAL_BATCH=0           a render pass per draw
#   no-cache      hw + RECOMP_METAL_SURFACE_CACHE=0   no retained surfaces
#   cap64         hw + RECOMP_METAL_BATCH_MAX=64      batches bounded at 64
#   rog           hw + RECOMP_METAL_SHADER_BLEND=3    every draw reads colour(0)
#   fmt565        hw + RECOMP_METAL_565=1             2-byte colour attachment
#   hw-cb, sw-cb  + RECOMP_METAL_CB_STATS/CB_GPU      command-buffer overlap
#
# depth-always and no-stencil render incorrectly BY CONSTRUCTION -- everything
# draws over everything, stencilled effects are not masked. They are
# diagnostics. Neither is ever evidence that the renderer is right; each
# answers one question, which is whether the artefact SURVIVES having that unit
# taken out of the picture.
#
# The last three are different in kind: each turns OFF an optimisation that is
# meant to be semantically invisible, so an arm that renders correctly is
# evidence the optimisation is not invisible after all. no-batch and no-cache
# are both defaults that changed on 15 Sep 2026; cap64 exists because an
# unbounded batch is the one case in which the vertex ring can wrap while the
# batch that reserved from it is still open.
#
# WHAT COMES OUT. Per arm, under
# build-macos/jsrf-first-fault/frame-bisect/<name>/<arm>/:
#   stderr.log      the whole run
#   fb/flipNNN.bmp  the live surface at each capture
#   fb/snapNNN.bmp  the FLIP_STALL copy -- the only one a person ever saw
#   ARM.txt         the switches, as the BACKEND reported them, not as set
#
# READ ARM.txt BEFORE READING THE FRAMES. It carries the [METAL] lines in which
# the backend names its own switch states. "I set the variable" and "the model
# read it" are different facts, and this repo has three recorded cases of an
# A/B whose control arm silently ran with the guard on.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: frame_bisect.sh <name> <arm>...}"
shift
[ $# -gt 0 ] || { echo "no arms given" >&2; exit 1; }

LIMIT="${BISECT_SECONDS:-280}"
PAD="${BISECT_PAD:-$ROOT/diagnostics/jsrf_first_fault/pad/gameplay_nobarrage.pad}"
# Presents between captures, and the wall-clock second to start capturing at.
# 105 s is when gameplay_nobarrage.pad first moves the stick; 120 leaves the
# loading screen and the opening camera behind it. The stride is deliberately
# small: 24 captures at 60 presents is about twenty seconds of play on this
# host, all of it inside the mission.
STRIDE="${BISECT_STRIDE:-60}"
AFTER="${BISECT_AFTER:-120}"

BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build-feav/jsrf_first_fault}"
[ -x "$BIN" ] || { echo "no binary at $BIN -- set JSRF_BIN" >&2; exit 1; }
[ -f "$PAD" ] || { echo "no pad file at $PAD" >&2; exit 1; }

OUT="$ROOT/build-macos/jsrf-first-fault/frame-bisect/$NAME"
mkdir -p "$OUT" || exit 1

# THE PIN. Re-hashed before every arm, and a change aborts the rest. On 15 Sep
# a rebuild landed underneath a running A/B and silently changed a compiled-in
# default halfway through; nothing about it was visible in the logs.
bin_hash() { openssl dgst -sha256 -r < "$BIN" | cut -d' ' -f1; }
PIN=$(bin_hash)

{
    echo "name:    $NAME"
    echo "arms:    $*"
    echo "binary:  $BIN"
    echo "sha256:  $PIN"
    echo "pad:     $PAD"
    echo "length:  ${LIMIT}s per arm, run one at a time"
    echo "capture: every $STRIDE presents once past t=${AFTER}s, 24 max"
    echo
} | tee "$OUT/RESULTS.txt"

arm_env() {
    # Every arm clears both bisect switches explicitly rather than leaving them
    # to the caller's shell. recomp_switch.h's whole point is that "" and "0"
    # are OFF, so an inherited RECOMP_METAL_HW_DEPTH_ALWAYS cannot leak into
    # the arm that is supposed to be the control.
    RECOMP_METAL_HW=0
    RECOMP_METAL_HW_DEPTH_ALWAYS=0
    RECOMP_METAL_HW_NO_STENCIL=0
    RECOMP_METAL_BATCH=1
    RECOMP_METAL_SURFACE_CACHE=1
    RECOMP_METAL_BATCH_MAX=0
    RECOMP_METAL_SHADER_BLEND=
    RECOMP_METAL_565=0
    RECOMP_METAL_CB_STATS=
    RECOMP_METAL_CB_GPU=
    RECOMP_METAL_DRAIN=0
    RECOMP_METAL_READBACK_AUDIT=0
    RECOMP_METAL_FENCE=0
    RECOMP_METAL_VSH=0
    case "$1" in
        sw)           ;;
        mode1)        RECOMP_METAL_HW=1; RECOMP_METAL_SHADER_BLEND=1 ;;
        hw)           RECOMP_METAL_HW=1; RECOMP_METAL_SHADER_BLEND=1 ;;
        hwdef)        RECOMP_METAL_HW=1 ;;   # identical to hw; named so the
                                             # RESULTS line says the DEFAULT
                                             # was what ran, with the variable
                                             # never set. The 15 Sep handover's
                                             # open item 3 was that no clean
                                             # capture had ever been taken
                                             # without it set explicitly.
        depth-always) RECOMP_METAL_HW=1; RECOMP_METAL_HW_DEPTH_ALWAYS=1 ;;
        no-stencil)   RECOMP_METAL_HW=1; RECOMP_METAL_HW_NO_STENCIL=1 ;;
        no-batch)     RECOMP_METAL_HW=1; RECOMP_METAL_BATCH=0 ;;
        no-cache)     RECOMP_METAL_HW=1; RECOMP_METAL_SURFACE_CACHE=0 ;;
        cap64)        RECOMP_METAL_HW=1; RECOMP_METAL_BATCH_MAX=64 ;;
        rog)          RECOMP_METAL_HW=1; RECOMP_METAL_SHADER_BLEND=3 ;;
        rog2)         RECOMP_METAL_HW=1; RECOMP_METAL_SHADER_BLEND=2 ;;
        rog0)         RECOMP_METAL_HW=1; RECOMP_METAL_SHADER_BLEND=0 ;;
        fmt565)       RECOMP_METAL_HW=1; RECOMP_METAL_565=1 ;;
        hw-cb)        RECOMP_METAL_HW=1; RECOMP_METAL_CB_STATS=1; RECOMP_METAL_CB_GPU=1 ;;
        sw-cb)        RECOMP_METAL_CB_STATS=1; RECOMP_METAL_CB_GPU=1 ;;
        drain)        RECOMP_METAL_HW=1; RECOMP_METAL_DRAIN=1 ;;
        rbaudit)      RECOMP_METAL_HW=1; RECOMP_METAL_READBACK_AUDIT=1 ;;
        fence)        RECOMP_METAL_HW=1; RECOMP_METAL_FENCE=1 ;;
        fence-sw)     RECOMP_METAL_FENCE=1 ;;
        gpuvsh)       RECOMP_METAL_HW=1; RECOMP_METAL_VSH=1 ;;
        gpuvsh565)    RECOMP_METAL_HW=1; RECOMP_METAL_VSH=1; RECOMP_METAL_565=1 ;;
        *) echo "unknown arm: $1" >&2; return 1 ;;
    esac
    export RECOMP_METAL_HW RECOMP_METAL_HW_DEPTH_ALWAYS RECOMP_METAL_HW_NO_STENCIL
    export RECOMP_METAL_BATCH RECOMP_METAL_SURFACE_CACHE RECOMP_METAL_BATCH_MAX
    # AN EMPTY EXPORTED VARIABLE IS NOT AN UNSET ONE, and for this switch the
    # difference is the difference between the fix and the worst arm. The
    # backend reads a VALUE here -- `e ? atoi(e) : 3` -- so an exported empty
    # string is a non-NULL getenv, atoi("") is 0, and mode 0 is the arm in
    # which NO draw reads the destination. `hwdef` exists to prove the DEFAULT
    # renders clean with the variable never set, so it has to actually be
    # unset. recomp_switch.h says the same thing one level down: "Empty counts
    # as off too, because `VAR= cmd` is how a shell unsets a variable for one
    # command" -- true there, and exactly the trap here.
    if [ -n "$RECOMP_METAL_SHADER_BLEND" ]; then
        export RECOMP_METAL_SHADER_BLEND
    else
        unset RECOMP_METAL_SHADER_BLEND
    fi
    export RECOMP_METAL_565
    export RECOMP_METAL_CB_STATS RECOMP_METAL_CB_GPU
    export RECOMP_METAL_DRAIN RECOMP_METAL_READBACK_AUDIT
    export RECOMP_METAL_FENCE
    export RECOMP_METAL_VSH
}

for ARM in "$@"; do
    arm_env "$ARM" || exit 1

    if [ "$(bin_hash)" != "$PIN" ]; then
        echo "ABORTING: the binary changed since this bisect started." >&2
        echo "  the arms already run are not comparable with the rest." >&2
        exit 4
    fi
    if pgrep -x jsrf_first_fault >/dev/null 2>&1; then
        echo "REFUSING: jsrf_first_fault is already running (pid $(pgrep -x jsrf_first_fault | tr '\n' ' '))" >&2
        exit 2
    fi

    ADIR="$OUT/$ARM"
    rm -rf "$ADIR"; mkdir -p "$ADIR/fb" || exit 1

    # play_scripted.sh is the ONE definition in this tree of how a scripted run
    # is started, and everything below is passed the way its own header says:
    # switches in the environment, JSRF_BIN for the binary. A private copy of
    # its environment would be measuring the divergence.
    #
    # NOTHING IS ADDED TO THAT ENVIRONMENT EXCEPT THE CAPTURE AND THE ARM, and
    # the two things deliberately NOT added are the two this script tried first.
    #
    #   SDL_AUDIODRIVER=no_such_driver   play_scripted.sh's header recommends
    #     it for anything not about audio, on a 14 Sep measurement of 2 of 3
    #     boots reaching New Game against 0 of 2 with the device live. That was
    #     a different tree. Run here on 16 Sep it took a guest SIGSEGV in
    #     DSOUND (sub_001A2E2E, EAX=FFFFFFB4) at t=41 s, in the state-12 title
    #     screen, and the arm produced no gameplay frame at all. Every run in
    #     this tree that HAS reached gameplay recently -- gameplay-hw,
    #     gameplay-sw, bis-dalways -- had the device live.
    #
    #   RECOMP_SEQ_REPORT=1   the scene gate ab_switch.sh insists on. It starts
    #     a 100 Hz sampler, and this title punishes instrumentation weight;
    #     none of the runs that reached gameplay carried it. The scene verdict
    #     is not lost, because this script's product is PICTURES: a frame of
    #     the mission is a stronger statement about which scene the run reached
    #     than a state number is. Look at the frames before believing an arm.
    # RETRY AN ARM THAT FAULTS BEFORE IT REACHES GAMEPLAY.
    #
    # A guest SIGSEGV in DSOUND -- sub_001A2E2E+0x670, guest 0xFFFFFFBE, EAX
    # FFFFFFB4 -- killed four of about twenty runs on 16 Sep 2026, every one of
    # them between t=34.0 and t=35.1 s, in the boot logos. It is not
    # deterministic: the same arm re-run immediately afterwards reaches
    # gameplay and finishes its 280 s. It is a real guest-correctness bug and
    # it has its own open item; until that is fixed it is a 20% tax on every
    # measurement, and a bisect that silently loses an arm to it is worse than
    # one that costs five more minutes.
    #
    # RETRYING IS SOUND HERE ONLY BECAUSE THE FAILURE IS TOTAL AND EARLY. The
    # arm produced no gameplay frame at all, so there is no result to bias by
    # choosing to re-run it -- this is not discarding an inconvenient number,
    # it is re-running a trial that did not happen. An arm that reaches
    # gameplay and THEN faults is kept and reported, because that is a result.
    ATTEMPT=1
    while : ; do
        echo "=== arm $ARM ($(date +%H:%M:%S))${ATTEMPT:+ attempt $ATTEMPT} ==="
        RECOMP_FB_DUMP="$ADIR/fb/" \
        RECOMP_FB_DUMP_FLIP="$STRIDE:$AFTER" \
        JSRF_BIN="$BIN" \
          "$ROOT/diagnostics/jsrf_first_fault/play_scripted.sh" \
            "frame-bisect-$NAME-$ARM" "@$PAD" "$LIMIT" > "$ADIR/launch.log" 2>&1

        SRC="$ROOT/build-macos/jsrf-first-fault/render-investigation/frame-bisect-$NAME-$ARM/stderr.log"
        [ -f "$SRC" ] && cp "$SRC" "$ADIR/stderr.log"

        GOT=$(ls "$ADIR/fb" 2>/dev/null | grep -c '^flip.*\.bmp$')
        FAULTED=$(grep -c 'FIRST GUEST FAULT' "$ADIR/stderr.log" 2>/dev/null || echo 0)
        [ "$GOT" -gt 0 ] && break
        [ "${FAULTED:-0}" -eq 0 ] && break
        [ "$ATTEMPT" -ge "${BISECT_RETRIES:-2}" ] && break
        echo "    arm $ARM faulted before gameplay -- retrying (attempt $ATTEMPT)"
        ATTEMPT=$((ATTEMPT + 1))
        rm -rf "$ADIR/fb"; mkdir -p "$ADIR/fb"
    done

    # The arm, as the BACKEND reported it. Everything here is a line the
    # runtime printed about its own state; nothing is echoed back from the
    # environment this script set.
    {
        echo "arm:     $ARM"
        echo "set:     RECOMP_METAL_HW=$RECOMP_METAL_HW"
        echo "         RECOMP_METAL_HW_DEPTH_ALWAYS=$RECOMP_METAL_HW_DEPTH_ALWAYS"
        echo "         RECOMP_METAL_HW_NO_STENCIL=$RECOMP_METAL_HW_NO_STENCIL"
        echo "         RECOMP_METAL_BATCH=$RECOMP_METAL_BATCH"
        echo "         RECOMP_METAL_SURFACE_CACHE=$RECOMP_METAL_SURFACE_CACHE"
        echo "         RECOMP_METAL_BATCH_MAX=$RECOMP_METAL_BATCH_MAX"
        echo "         RECOMP_METAL_SHADER_BLEND=${RECOMP_METAL_SHADER_BLEND-<unset, backend default>}"
        echo "         RECOMP_METAL_565=$RECOMP_METAL_565"
        echo
        echo "-- what the backend said it was doing --"
        grep -E '^\[METAL\] (hw draws|bisect|one encoder|shader blend|colour attachment|MIXED)' \
            "$ADIR/stderr.log" 2>/dev/null | tail -12
        echo
        echo "-- did the run survive --"
        grep -A6 'FIRST GUEST FAULT' "$ADIR/stderr.log" 2>/dev/null | head -8
        grep '\[FB\] t=' "$ADIR/stderr.log" 2>/dev/null | tail -1
        echo
        echo "-- captures --"
        grep '\[FLIP\] capture' "$ADIR/stderr.log" 2>/dev/null | tail -30
    } > "$ADIR/ARM.txt" 2>&1

    N=$(ls "$ADIR/fb" 2>/dev/null | grep -c '^flip.*\.bmp$')
    TRIS=$(grep '\[GPU\] .* triangles rasterised' "$ADIR/stderr.log" 2>/dev/null | tail -1)
    LAST=$(grep '\[FB\] t=' "$ADIR/stderr.log" 2>/dev/null | tail -1 | sed 's/ *\[FB\] \(t= *[0-9.]*\).*/\1/')
    FAULT=$(grep -c 'FIRST GUEST FAULT' "$ADIR/stderr.log" 2>/dev/null)
    {
        echo "arm $ARM: $N gameplay frame(s) captured, ran to ${LAST:-t=?}"
        [ "${FAULT:-0}" -gt 0 ] && echo "  *** GUEST FAULT -- this arm ended early and proves nothing ***"
        [ "$N" -eq 0 ] && echo "  *** NO FRAME PAST t=${AFTER}s -- this arm proves nothing ***"
        echo "  ${TRIS:-  (no triangle report)}"
        grep '^\[METAL\] bisect:' "$ADIR/stderr.log" 2>/dev/null | tail -1 | sed 's/^/  /'
        grep '^\[METAL\] hw draws' "$ADIR/stderr.log" 2>/dev/null | tail -1 | sed 's/^/  /'
        echo
    } | tee -a "$OUT/RESULTS.txt"
done

echo "frames under $OUT/<arm>/fb/"
echo "READ <arm>/ARM.txt FIRST -- it says what the backend thought it was doing."
