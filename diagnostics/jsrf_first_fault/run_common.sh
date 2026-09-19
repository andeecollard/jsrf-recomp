# Shared preconditions for every measured run.  Source this; do not execute it.
#
# Two lessons here were each paid for with a batch of discarded runs, and both
# used to depend on somebody remembering:
#
#   1. Rebuild before measuring.  A translator fix reached no binary for a day
#      and 440 uninitialised reads shipped anyway; separately, six A/B runs
#      were lost because a source file was edited while they ran.  The check
#      is mechanical now, so forgetting is not one of the outcomes.
#
#   2. Know what the guest read from disc.  Every run copies the emulated HDD
#      to scratch so the title can write to it, which means the thing under
#      measurement is the copy, not the source.  "Same initial HDD contents"
#      was asserted from the fact that the copy came from the same directory
#      -- an mtime-and-provenance argument, not a content one.  A source that
#      changed between two runs of a pair would be invisible to it.  So hash
#      the source, hash the copy, and record both in the run directory.
#
# Callers set ROOT, BIN, OUT and SCRATCH first, then call:
#     jsrf_require_current_binary
#     jsrf_require_idle
#     jsrf_stage_hdd
# Escape hatches are explicit: JSRF_ALLOW_STALE=1, JSRF_SKIP_HDD_MANIFEST=1.

# WHICH DISK DOES THE TITLE BOOT FROM, AND IS ITS CACHE WARM?
#
# A console keeps the Z: cache across launches of the same title and formats it
# only when a DIFFERENT title launches, so the player's steady state is a WARM
# cache. Every run in the 782-run corpus booted from the stock tree, whose
# Cache/ is empty, and so replayed a FIRST launch the player never performs:
# 1,410 file opens rebuilding 258 files and 119 MB of media cache, against 131
# opens warm, and about 30 s of guest time. Measured 19 Sep 2026; it is why
# replaying the player's own recording from the stock tree landed ~30 s behind
# their session with the input stream identical.
#
# A WARM TREE IS THE DEFAULT, and the obvious objection was measured and is
# wrong. Every schedule in pad/ is keyed to wall-clock seconds, so the fear was
# that a boot 30 s shorter would land the presses in the wrong places. Two
# 240 s runs of gameplay_nobarrage.pad, same binary, same schedule:
#
#     cold   1,413 opens   off=271 retired voices
#     warm     131 opens   off=281
#
# No penalty; if anything the warm arm plays slightly more. The scare came
# from a pair of 110 s runs that both read off=10 -- COLD as well as warm --
# because this schedule's presses run to t=96 s and 110 s is simply too short
# to accumulate voices. Run length, not disk. (For scale, today: 110 s -> 10,
# 150 s -> ~90, 240 s -> ~270, 300 s -> 362.)
#
# JSRF_HDD_SRC set by the caller still wins, and pointing it at the cold tree
# is how a first launch is measured deliberately.
#
# Neither tree is beside the repo on this machine -- the repo was moved out of
# iCloud and the 4.9 GB trees were not -- so both known locations are tried
# rather than one being assumed. Neither is vendored and neither can be.
JSRF_HDD_WARM="${JSRF_HDD_WARM:-}"
if [ -z "$JSRF_HDD_WARM" ]; then
    for _c in "$HOME/jsrf-build/emulated-hdd-warm" \
              "$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/emulated-hdd-warm"; do
        if [ -d "$_c" ]; then JSRF_HDD_WARM="$_c"; break; fi
    done
fi
JSRF_HDD_COLD="${JSRF_HDD_COLD:-}"
if [ -z "$JSRF_HDD_COLD" ]; then
    for _c in "$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd" \
              "$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd"; do
        if [ -d "$_c" ]; then JSRF_HDD_COLD="$_c"; break; fi
    done
fi
JSRF_HDD_SRC="${JSRF_HDD_SRC:-${JSRF_HDD_WARM:-$JSRF_HDD_COLD}}"

# Say which tree, and whether its cache is warm. A boot-time number read
# without this line is not comparable with one read from a different tree.
jsrf_say_hdd() {
    if ls "$JSRF_HDD_SRC"/Cache/Media/Cache/*COMPLETE* >/dev/null 2>&1; then
        echo "hdd:      $JSRF_HDD_SRC (cache WARM -- the title skips its cache build)"
    else
        echo "hdd:      $JSRF_HDD_SRC (cache COLD -- the title spends ~30 s rebuilding it)"
    fi
}

# Where the title itself lives.  main.c falls back to a relative "game/", which
# exists nowhere, so a script that forgets to pass this gets "failed to load
# XBE" and an empty run -- run_scripted.sh did exactly that, and only worked
# when the caller's shell happened to have RECOMP_XBE_PATH exported already.
GAME_DIR="${JSRF_GAME_DIR:-$ROOT/../Jet Set Radio Future (US)}"
jsrf_require_game() {
    if [ ! -f "$GAME_DIR/default.xbe" ]; then
        echo "no default.xbe under $GAME_DIR" >&2
        echo "  set JSRF_GAME_DIR to the directory holding it" >&2
        exit 1
    fi
    RECOMP_XBE_PATH="$GAME_DIR/default.xbe"; export RECOMP_XBE_PATH
    RECOMP_GAME_DIR="$GAME_DIR";             export RECOMP_GAME_DIR
}

# Sources that are actually linked into the game binary.  The first version of
# this guard also matched *_test.c, which is built into the ctest executables
# and not into jsrf_first_fault, so touching a test refused every run for no
# reason.  A check that fires when nothing is wrong is one people switch off.
# " 2.c" is iCloud's conflicted-copy naming, never a real source.
jsrf_binary_sources() {
    find "$ROOT/src" "$ROOT/diagnostics/jsrf_first_fault" \
         \( -name '*.c' -o -name '*.h' -o -name '*.m' \) 2>/dev/null \
      | grep -vE '_test\.(c|m)$' | grep -v ' 2\.c$'
}

jsrf_require_current_binary() {
    if [ ! -x "$BIN" ]; then
        echo "no binary at $BIN" >&2
        echo "  build it:  cmake --build ${BIN%/*} -j 8" >&2
        exit 1
    fi
    NEWER=$(jsrf_binary_sources | while read -r f; do [ "$f" -nt "$BIN" ] && echo "$f"; done | head -5)
    if [ -n "$NEWER" ]; then
        echo "REFUSING: $BIN is older than these sources -- you would be measuring the previous build:" >&2
        echo "$NEWER" | sed 's/^/    /' >&2
        echo "  rebuild:   cmake --build ${BIN%/*} -j 8" >&2
        echo "  (JSRF_ALLOW_STALE=1 overrides, and invalidates the run)" >&2
        [ -n "${JSRF_ALLOW_STALE:-}" ] || exit 1
    fi
}

# TWO processes can be this title, and the guard only ever knew about one.
#
# A scripted run is jsrf_first_fault. A PERSON playing is jsrf-engine, inside
# JSRF.app -- a different name for the same game, and pgrep -x does not match
# it. So the check passed cleanly while a human was mid-session, which is how
# an agent's run and a playtest collided twice on 19 Sep 2026. The second one
# cost the player a launch that never came up; a run of this title saturates
# the CPU, and the session beside it is both a bad experience and a worthless
# measurement.
jsrf_require_idle() {
    if pgrep -x jsrf_first_fault >/dev/null 2>&1; then
        echo "REFUSING: jsrf_first_fault is already running (pid $(pgrep -x jsrf_first_fault | tr '\n' ' '))" >&2
        exit 2
    fi
    if pgrep -x jsrf-engine >/dev/null 2>&1; then
        echo "REFUSING: somebody is PLAYING (jsrf-engine pid $(pgrep -x jsrf-engine | tr '\n' ' '))." >&2
        echo "  Only one of this title may run at a time, and a scripted run" >&2
        echo "  would saturate the CPU under them. Wait for them to finish." >&2
        exit 2
    fi
}

# ...and checking once, at the start, is not enough either.
#
# The collision that actually happened went the other way round: the run
# started on an idle machine and the human launched two minutes later. Nothing
# made the run stand down, so it kept the CPU for its full limit. This watches
# for a player appearing MID-RUN and gets out of their way, leaving a marker
# so the trial is discarded rather than scored -- a run that shared the
# machine with a playtest measures the contention, not the change under test.
#
# Call it after the run starts, with the run's pid; kill the returned pid when
# the run ends.
jsrf_yield_to_player() {
    _run_pid=$1; _out=$2
    ( while kill -0 "$_run_pid" 2>/dev/null; do
          if pgrep -x jsrf-engine >/dev/null 2>&1; then
              echo "YIELDING: a player launched JSRF.app; abandoning this run" >&2
              echo "a player launched mid-run; this trial measures contention" \
                  > "$_out/YIELDED_TO_PLAYER"
              kill -TERM "$_run_pid" 2>/dev/null
              sleep 5; kill -9 "$_run_pid" 2>/dev/null
              return 0
          fi
          sleep 2
      done ) &
    echo $!
}

# Content, not provenance: every regular file's size and SHA-256, sorted, so
# two runs' manifests can be diffed directly.  The copy is hashed after it is
# made, which is what the guest will actually read.
#
# openssl rather than shasum, which is Perl and measured 305 MB/s against
# openssl's 1.7 GB/s on this machine.  The emulated HDD is 5.2 GB and gets
# hashed twice per run, so that is the difference between a 6-second check and
# a 34-second one, and a 34-second one is a check people start skipping.
jsrf_hdd_manifest() {
    ( cd "$1" && find . -type f -print0 \
        | LC_ALL=C sort -z \
        | xargs -0 openssl dgst -sha256 -r 2>/dev/null )
}

jsrf_stage_hdd() {
    if [ ! -d "$JSRF_HDD_SRC" ]; then
        echo "no emulated HDD at $JSRF_HDD_SRC" >&2
        echo "  set JSRF_HDD_SRC to the directory holding it" >&2
        exit 1
    fi
    cp -R "$JSRF_HDD_SRC" "$SCRATCH/hdd" || exit 1
    [ -n "${JSRF_SKIP_HDD_MANIFEST:-}" ] && return 0

    jsrf_hdd_manifest "$JSRF_HDD_SRC" > "$OUT/hdd-source.manifest"
    jsrf_hdd_manifest "$SCRATCH/hdd"  > "$OUT/hdd-copy.manifest"
    if ! cmp -s "$OUT/hdd-source.manifest" "$OUT/hdd-copy.manifest"; then
        echo "REFUSING: the disposable HDD copy does not match its source" >&2
        diff "$OUT/hdd-source.manifest" "$OUT/hdd-copy.manifest" | head -20 >&2
        exit 3
    fi
    echo "hdd:    $(wc -l < "$OUT/hdd-source.manifest" | tr -d ' ') files, sha256 of manifest $(shasum -a 256 < "$OUT/hdd-source.manifest" | cut -c1-16)"
}
