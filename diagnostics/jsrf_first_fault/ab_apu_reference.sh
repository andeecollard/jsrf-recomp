#!/bin/sh
# Does the APU voice path behave BETTER with our additions switched off?
#
# WHY THIS EXISTS. xemu's entire voice-list walk is:
#
#     for (int i = 0; d->regs[current] != 0xFFFF; i++) {
#         if (i >= MCPX_HW_MAX_VOICES) break;
#         uint16_t v = d->regs[current];
#         d->regs[next] = voice_get_mask(d, v, ..._NEXT_VOICE_HANDLE);
#         if (!voice_get_mask(d, v, ..._ACTIVE_VOICE)) fe_method(d, SE2FE_IDLE_VOICE, v);
#         else voice_work_enqueue(d, v, list);
#         d->regs[current] = d->regs[next];
#     }
#
# It raises for every inactive voice every frame, unconditionally: no
# coalescing, no edge latch, no self-link termination, no held decode pair. It
# clobbers FEDECPARAM for each subsequent inactive voice and does not care. Our
# walk has 55 references to coalescing, suppression and latching, and every one
# was added to fix a symptom.
#
# The handover records that level-triggered raising "breaks up the music" --
# but level-triggered raising is what the reference does, and the reference
# runs this title. That inverts the question: perhaps the additions, not the
# raising, are what strand voices.
#
# THIS IS A FOUR-SWITCH ARM ON PURPOSE, which is normally forbidden here. It is
# a PROBE, not a measurement of any one switch: if reference mode keeps the
# audio alive, bisecting the four is the next job and is worth doing. If it
# does not, the divergence is exonerated in six runs instead of an evening.
#
# Usage: ab_apu_reference.sh [trials] [seconds]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TRIALS=${1:-3}
SECS=${2:-240}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$ROOT/build-macos/jsrf-first-fault/measure/apuref_ab_$STAMP"
mkdir -p "$OUT"

# Held identical across both arms: the renderer, and the things not under test.
COMMON="RECOMP_METAL_FF=1 RECOMP_VSH_DP_ZERO=1 RECOMP_NV2A_PMC_UPMIRROR=1 RECOMP_APU_LIST_MOVE_TO_FRONT=1"
# ARM `ref`: the four additions off, which is xemu's behaviour.
REF="RECOMP_APU_TRAP_COALESCE=0 RECOMP_APU_IDLE_TRAP_EDGE=0 RECOMP_APU_SELFLINK_END=0 RECOMP_APU_FEDEC_HOLD=0"
# ARM `cur`: the player's configuration as it ships today.
CUR="RECOMP_APU_IDLE_TRAP_EDGE=1 RECOMP_APU_SELFLINK_END=1 RECOMP_APU_FEDEC_HOLD=1"

echo "trials:  $TRIALS x ${SECS}s x 2 arms"
echo "ref arm: $REF"
echo "cur arm: $CUR"
echo "out:     $OUT"

i=1
while [ "$i" -le "$TRIALS" ]; do
    for arm in ref cur; do
        eval "SET=\$$(echo $arm | tr a-z A-Z)"
        name="t${i}_apu${arm}"
        echo "--- $name ---"
        # play_scripted.sh unmodified, one definition of how a run starts.
        ( eval "export $COMMON $SET"; \
          sh "$ROOT/diagnostics/jsrf_first_fault/play_scripted.sh" \
             "$name" "@$ROOT/diagnostics/jsrf_first_fault/pad/gameplay.pad" "$SECS" \
             > "$OUT/$name.out" 2>&1 ) || true
        L="$ROOT/build-macos/jsrf-first-fault/render-investigation/$name/stderr.log"
        [ -f "$L" ] || { echo "  no log"; continue; }
        cp "$L" "$OUT/$name.stderr.log" 2>/dev/null || true
        # THE METRIC IS AUDIO SURVIVAL, not frame time. A run whose 2D heard is
        # still advancing in its last window kept playing music; one whose
        # guest_methods froze stopped talking to the APU. Frame time cannot see
        # either, which is why the existing scorer is the wrong tool here.
        tail2=$(grep -ao "2D heard=[0-9]*" "$L" | tail -2 | sed 's/.*=//' | tr '\n' ' ')
        gm=$(grep -aoE "guest_methods=[0-9]+" "$L" | uniq -c | tail -1 | awk '{print $1"x"}')
        dl=$(grep -ao "\[APU-IDLE-DELIVERY\][^\\n]*" "$L" | tail -1)
        sc=$(grep -ao "SCENE:[^\\n]*" "$L" | tail -1)
        printf "  %-14s 2Dheard(last2)=%s guest_methods frozen=%s\n" "$name" "$tail2" "$gm"
        printf "      %s\n" "${dl:-no delivery line}"
        printf "      %s\n" "${sc:-scene unknown}"
    done
    i=$((i + 1))
done
echo "=== logs: $OUT ==="
