# The defer-swap A/B, and the switch that refuses itself — 17 Sep 2026, night

`RECOMP_METAL_DEFER_SWAP`, three trials per arm, 240 s, binary pinned by
sha256, ABBA order, 20 s mission warmup. Run directory:
`build-macos/jsrf-first-fault/measure/defer_ab_20260917-215715`.

## The frame times say nothing

    RECOMP_METAL_DEFER_SWAP=0   25.40  18.42  19.40 ms   (mean 21.07, n=3)
    RECOMP_METAL_DEFER_SWAP=1   39.67  20.47        ms   (mean 30.07, n=2)
    excluded: t1_defer1 never reached a mission (scene=12)

    THE RANGES OVERLAP. The difference of means (9.00 ms) is inside the
    run-to-run spread.

**Arms verified distinct** — but only after fixing the scorer; see below.

## Why they say nothing: the switch is a no-op in gameplay

This is the result, and it is a mechanism rather than a frame-time argument.
Per run, from the `[METAL]` line:

    t1_defer1   deferred: 1   refused: 5747  depth dirty,  0 no slot
    t2_defer1   deferred: 1   refused: 7295  depth dirty,  0 no slot
    t3_defer1   deferred: 1   refused: 12656 depth dirty,  0 no slot

**One deferral per run, and thousands of refusals, every one of them for
depth.** `nv2a_metal.m:3642` reads `if(depth_dirty){++g_swap_defer_depth;}` —
A2 refuses to defer whenever depth is dirty, because `surface_slot_writeback`
carries colour only and a slot's depth is retained for a rebind but never
written to guest RAM.

In a mission depth is dirty at essentially every swap. So the deferral A2 was
built to perform almost never happens, the swap keeps paying its full drain
and read-back, and `[SYNC]` stays where it was. The A/B did not measure
deferring against not deferring; it measured not-deferring against
not-deferring, twice.

That also disposes of the on arm looking *slower*. With the mechanism inert,
30.07 against 21.07 is spread — and `t2_defer1` carries `busiest=96%`, which
is a loaded host, not a switch.

## What this means for G3

**A2 as built cannot deliver `[NOSYNC] p50 = 8.0 ms`.** The blocker is not the
deferral mechanism, which works — the one deferral per run proves the path is
live and the test already pins both arms. The blocker is **depth**.

So G3's question changes from *"does deferring the swap help"* to:

- Why is depth dirty at nearly every swap in a mission?
- Does guest RAM ever need that depth? A slot's depth is retained on the GPU
  for the rebind, and the refusal exists only because `surface_slot_writeback`
  cannot carry it. If nothing reads depth from guest RAM, the refusal is
  protecting a value nobody consumes.
- If something does read it, can depth be written back on the same
  range-aware terms A1 already established for colour?

`no slot` refusals are **0** across all three runs, so the other refusal path
is not in play at all.

**Do not re-run this A/B before that question is answered.** Six more runs
would re-measure the same inert switch.

## A harness bug that hid four switches

The scorer first reported `arms NOT verified distinct: no report names
RECOMP_METAL_DEFER_SWAP`, and the comparison ran anyway with the identical-arms
VOID check silently skipped. `ab_score.py`'s own comment beside the
registration asserted the opposite:

> G3 A2. Token printed unconditionally on the `[METAL]` swap-deferral line, so
> the off arm has something to match and the VOID check can run.

The token *is* printed unconditionally. The harvester could not see it, for two
independent reasons:

    METAL_SWITCH_RE = re.compile(r"\[METAL\][^(]*\((metal_(?:hw|565|batch) \w+)\)")

the token list was hardcoded to three names, and `[^(]*` stops at the **first**
parenthesis — which on

    [METAL] swap writeback deferred: 1 (refused: 5747 depth dirty, 0 no slot) (defer_swap on)

is `(refused: ...)`, so the alternation failed before it reached `defer_swap`.

This is the same defect that was found and fixed for `RECOMP_SYNC_HIST` earlier
the same day and **not generalised**. Now generic — every `(token on)` or
`(token OFF)` group anywhere on a `[METAL]` line, harvested with `findall`
rather than `search` so one line can carry several.

It was hiding **four** switches, not one. Re-scoring the same logs with the fix
harvests `defer_swap`, and also `area_double`, `metal_vsh` and `surface_cache`,
none of which had ever appeared in a `switch{}` block. **Any A/B taken on those
three was scored with its VOID check skipped too.**

Re-scored from the untouched logs, with no new runs:

    arms verified distinct: RECOMP_METAL_DEFER_SWAP=0 reported "defer_swap OFF",
                            =1 reported "defer_swap on"

## The one run that did not reach a mission

`t1_defer1` stopped at scene=12 with `flips=0` — no frames presented at all.
It is the arm under test, which makes it worth naming, but it is **one
occurrence** and the harness excludes it by design. The other two `defer=1`
runs reached scene 30 and presented 4,288 and 8,797 flips. Boot safety is not
measured by this A/B and the scorer says so itself.
