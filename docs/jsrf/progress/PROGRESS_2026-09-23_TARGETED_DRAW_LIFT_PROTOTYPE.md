# Targeted DrawVertices lift: a working interception experiment

The user asked to try the targeted graphics-replacement approach discussed
for Burnout 3. This experiment replaces one JSRF US guest draw routine with
native C, behind `RECOMP_JSRF_DRAW_LIFT=1`, in a separate generated tree.
The installed application and original generated sources are unchanged.

## What is replaced

`sub_00199300` is the active US build's non-indexed DrawVertices entry.
The earlier semantic-recovery experiment used another XBE whose corresponding
entry is `0x001992C0`. Do not transfer those addresses between binaries.

The replacement preserves the original validation call (`0x00196520`),
reservation call (`0x001916C0`), guest stack layout, return addresses, saved
registers and packet order. Its loop emits DRAW_ARRAYS batches of at most 256
vertices directly in native C. Writes retain the original memory-watch PCs.
Counts outside 1–65536 or ranges outside the 24-bit index domain fall back to
the original generated routine. The switch is off by default.

**This is an interception/ABI prototype, not a direct Metal renderer.** It
still emits the original pushbuffer and uses the current renderer. It cannot
answer whether bypassing NV2A translation would outperform G27, and it does
not remove software texture sampling.

## Evidence

- 420 differential cases compare the compiled generated routine against the
  native replacement: complete 8 MB test memory, general registers, stack
  cleanup, helper order/arguments, state writes and relocated reservations.
  Boundary cases include 255/256/257 vertices and 65536 vertices; random
  cases use a fixed seed. The comparison passed with UBSan enabled.
- Five wrapper cases verify valid dispatch and invalid-range fallback, with
  the switch both off and on. The fallback is a spy in these wrapper tests;
  invalid inputs are not claimed to be safe in the original routine.
- Thirty cases execute the **original US XBE machine code in Unicorn** and
  compare it against the native implementation, including complete memory
  and general registers. Both use helper doubles. This does not prove the
  helpers themselves correct or verify arbitrary mid-function entry.
- A separate Release game executable built successfully from the staged gen.
- Two 60-second GPU smoke arms used the same executable and separate cloned
  HDDs. Both initialized the Apple M1 Max Metal pipeline. The native arm
  logged at least **4096 intercepted calls**. Both reached scene reports
  with `live=60`; neither establishes tutorial gameplay from that number.
  The observed scene transitions differ, so cumulative frame times must not
  be interpreted as an A/B speedup. No image-equality claim is made.

The initial sandboxed smoke run fell back to software because Metal did not
initialize. It is excluded from GPU conclusions. Unicorn also required an
unsandboxed run for executable-memory setup. Apple system Python could not
load the UBSan library, so the original-XBE comparison used `--no-sanitize`;
the separate native/generated comparison ran with UBSan under Python 3.13.

## Reproduce

From the repository root, using the current US gen:

```sh
python3 diagnostics/jsrf_first_fault/check_draw_vertices_lift.py /path/to/gen/recomp_0008.c
RECOMP_JSRF_DRAW_LIFT=1 python3 diagnostics/jsrf_first_fault/check_draw_vertices_lift.py /path/to/gen/recomp_0008.c
# With Unicorn and Capstone installed in this Python environment:
RECOMP_JSRF_DRAW_LIFT=1 python3 diagnostics/jsrf_first_fault/check_draw_vertices_lift.py /path/to/gen/recomp_0008.c --xbe /path/to/default.xbe

python3 diagnostics/jsrf_first_fault/stage_draw_vertices_lift.py /path/to/gen /tmp/jsrf-draw-experiment/gen
cmake -S diagnostics/jsrf_first_fault -B /tmp/jsrf-draw-experiment/build -DRECOMP_GEN_DIR=/tmp/jsrf-draw-experiment/gen -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/jsrf-draw-experiment/build --target jsrf_first_fault -j 6
python3 diagnostics/jsrf_first_fault/run_draw_vertices_lift.py /tmp/jsrf-draw-experiment/build/jsrf_first_fault /path/to/game /path/to/hdd /tmp/jsrf-draw-experiment/smoke
```

Staging rejects an existing destination, a destination within the source,
or a generated function whose SHA-256 does not match the inspected body. It
copies rather than modifies the original. The XBE oracle separately pins the
159-byte US routine by SHA-256. No game code or assets are added to source control.

Local artifacts: `/tmp/jsrf-draw-lift-20260923/` (staged gen, build and logs).
The GPU logs are `gpu-smoke/arm0/run.log` and `gpu-smoke/arm1/run.log`.
The build includes the pre-existing uncommitted runtime changes in the
working tree; this is not a clean-commit benchmark.

## What the next experiment must establish

1. Recover the indexed draw entry and measure coverage by draw type. This
   non-indexed routine alone is not evidence that most rendering is covered.
2. Define a state/ordering bridge before replacing commands with host draws.
   Existing research in `CODEX_HANDOVER.txt`, sections (x) and (y), shows
   render-state setters inlined in JSRF game code and other state emitted
   through separate paths. A draw-only hook cannot ignore that pending work.
3. Feed a bounded supported draw to the existing Metal backend, with an exact
   fallback for unsupported state. Compare captured output for the same
   scene, then measure the remaining CPU/GPU cost.

This gives us a tested place to start that work. It does not justify replacing
the current renderer wholesale or withdrawing G27's hardware-texture work.
