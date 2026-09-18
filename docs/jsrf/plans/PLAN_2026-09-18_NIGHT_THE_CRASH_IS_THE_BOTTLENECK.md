# The crash is the bottleneck — 18 September 2026, evening

Written after a day that shipped one player-confirmed improvement, killed six
hypotheses, and found that the thing blocking everything else was never on the
list.

## The scoreboard

| defect | player sees | status tonight |
|---|---|---|
| speed | 15.5→21 ms across a session | **9.1% shipped, player-confirmed clean** |
| music | dies ~2 min in, every session | open; delivery and cycle both closed |
| text | lines above letters, wrong glyphs | open; 4 mechanisms dead, trap re-armed |
| **crashes** | occasional | **destroying half of every measurement** |

## Why the crash goes first now

Eight crashed runs today, every one `sub_001A2E2E`, and they cost:
`nodepth_ab` its control arm (VOID), `deferdepth_ab` three control runs (VOID),
`deferdepth2_ab` six trials per arm to yield two and four, and the 400 s
sync-histogram run its entire purpose.

**Every VOID verdict this project printed today traces to it.** Fixing it
roughly doubles the value of every run, which is worth more than any single
measurement those runs were trying to take. It is also the only defect on the
list that is fully traced to an instruction.

## Track A — capture `this`, then guard honestly *(first)*

**A1. Probe a gen copy for the DirectSound object pointer.** The ISR chain is
guest code we generate. A probe at `001A241F` or `001A200D` captures `this`
once. Install into a **copy** of the gen tree with a script in
`diagnostics/jsrf_first_fault/`, per CLAUDE.md — never committed into a fix.
APFS `cp -c` makes the copy near-free; only the affected translation units
rebuild.

**A2. Read `owner[h]` before raising.** With `this` known, the model reads
`this + 0x2C4 + h*4` and declines to raise for a handle whose owner is NULL.
That is not a workaround: it makes our raises honour the invariant the ISR is
documented as entitled to rely on — *"a handle only reaches this chain if
DirectSound put the voice in a list, which it does after it has an owner"*.

*Risk:* `this` may be per-device; the probe must catch the right one.
*Control:* count suppressed raises. A run where it suppresses nothing and still
crashes kills this too.

**A3. Only then, the A/B.** With the crash gone, every subsequent measurement
costs half what it does today.

## Track B — the audio, with two doors closed

Delivery is settled: **15 of 15 handles delivered, twice**, on sessions where
the music demonstrably died. The guest is told and does not act. Cycles are
settled: the music dies with `walks_with_a_cycle=0`, so a ring is not
necessary.

**B1. Read the VOICE-TOP ring.** Built today, armed in the player's config,
never yet read. It captures the ~12 *successful* head removals a session — the
only positive examples of the behaviour we want, against 100,000 failures.
**This needs a player session and nothing else.**

**B2. The `relink` signal.** 46% of voice-list head inserts are for a handle
already in the list, and the player independently reported "fx repeating in
weird ways". That is the first time a symptom and a counter have met. Worth a
per-voice breakdown once B1 has been read.

## Track C — the text, with the trap finally working

Four mechanisms are dead (texture matrix, texture formats, combiner alpha,
glyph-batch vertex changes). The FF path is now known to be a composite
transform and nothing else.

**C1. Read the re-armed frame watch.** Keyed on surface address, eight slots,
with `comparisons=` as its positive control. **Never yet run.** Until it
produces a `comparisons>0, N changed`, G2 has no evidence at all beyond the
player's eyes.

*Order note:* C1 is nearly free and it is the only thing that can make the text
defect reproducible headless. Do it on the next run of anything.

## Track D — G3's remaining gap

`no_depth_sync` is in and player-confirmed. The session still runs 15.5 ms for
~35 windows and then degrades to 21 ms, and **the storm does not explain it** —
the storm climbs linearly from the start while frame time is flat and only then
climbs.

**D1. Find what changes at the onset.** `RECOMP_SYNC_HIST` is the per-frame
stage breakdown. Today's attempt died to the crash at 6.4 s. **This is blocked
on Track A**, which is the argument for Track A in one line.

## What this plan does not do

- **No new audio theory.** Seven have died. B1 is a read of a capture that
  already exists.
- **No shipping `defer_swap`.** Measured 8.2% *slower*, non-overlapping. The A1
  narrowing stays because it costs nothing off and it is what made the
  measurement possible.
- **No upstream merge.** Unchanged.
- **No more A/Bs until the crash is fixed.** Today proved they cost double and
  VOID at n=1 vs 3. That is not a budgeting preference; it is arithmetic.

## Upstream, for completeness

PR #67 (mixbin) and #69 (narrow rotates) are open, zero comments. The
maintainer has not pushed since 16 Sep and the queue is ten deep. Nothing to
do. G15 (`bts`/`btr`/`btc` CF) and `get_data_ptr` remain owed as **issues**
rather than patches.
