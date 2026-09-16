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

JSRF_HDD_SRC="${JSRF_HDD_SRC:-$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd}"

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

jsrf_require_idle() {
    if pgrep -x jsrf_first_fault >/dev/null 2>&1; then
        echo "REFUSING: jsrf_first_fault is already running (pid $(pgrep -x jsrf_first_fault | tr '\n' ' '))" >&2
        exit 2
    fi
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
