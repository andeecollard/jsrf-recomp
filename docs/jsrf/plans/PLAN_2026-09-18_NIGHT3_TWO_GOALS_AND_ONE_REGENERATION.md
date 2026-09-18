# Two goals, and one regeneration — 18 September 2026, night (3)

Written after a day that refuted G1's founding invariant, answered D1 for free,
and read Microsoft's own static recompiler for the same console across five
agents. Supersedes the ordering in
`PLAN_2026-09-18_NIGHT2_G1_IS_REFUTED_AND_THE_GUEST_STOPS_ALLOCATING.md`; that
plan's findings stand and its §4b is folded in here.

The project has two goals and they are not the same work:

> **A. Get JSRF running properly.** The player's words: everything on the GPU,
> 60 fps, correct speed.
>
> **B. Improve xboxrecomp.** A better recompiler, upstreamable, not JSRF-shaped.

Most of today's leverage is on A. The BC research is mostly on B. Keeping them
apart stops B's interesting work from displacing A's de-risked work, which is the
rule that already cost this project a day.

## THE PLANNING FACT THAT REORDERS EVERYTHING

**Work in `src/` is cheap. Work in `tools/` is expensive.**

`src/nv2a/*`, `src/apu/*`, `src/kernel/*` compile in minutes against an existing
gen tree. Anything in `tools/recomp/` — the lifter, the translator — requires a
**full regeneration**, which CLAUDE.md warns is not bit-stable across translator
changes and which invalidates every archived baseline and every A/B measured
against one.

So:

- **Every GPU improvement BC suggested is `src/` work.** No regeneration.
- **Every CPU improvement BC suggested is `tools/` work.** One regeneration.

Therefore: do the GPU work now, incrementally, measuring each step. **Batch every
translator change into ONE regeneration**, done once, with a re-verified
baseline. Do not spend a regeneration on a single idea.

---

## 0. THE SESSION — gated on the player, nothing else answers it

`RECOMP_APU_TRAP_THREADS` is armed alongside `RECOMP_KERNEL_THREADS`. Together
they fork G1:

- submitting thread's `vp=` stops growing while `[KERNEL-THREADS]` shows it still
  calling ⇒ **the guest stopped asking**, and the question is its audio code;
- both stop together ⇒ **it stopped being scheduled**, and the question is ours.

**Reach actual gameplay.** The 17:55 session sat at `on=27`, well below the
148–453 band, so it never exercised the scene where the black screen appeared.
`RECOMP_GUEST_NAMES` is armed too, so any crash now names the code around it.

**Done when:** G1 is one of those two things and not the other.

---

## GOAL A — GET JSRF RUNNING

### A1. The NV2A method path *(`src/`, no regeneration, biggest lever)*

From `docs/technical/ms-fusion-nv2a-translator.md`. Microsoft's translator
targets **D3D9, not GPU registers** — an API-level translation like ours, so this
transfers far better than a hardware model would.

1. **Replace `pb_exec_method_body`'s ~900-line `switch` with a descriptor
   table.** Theirs is 2,048 entries covering `0x0000–0x1FFC`, indexed by raw
   method offset, no hashing, no bounds check beyond a mask; entries are packed
   descriptors selecting one of ~34 handler bodies. The dirty group becomes
   **table data rather than control flow**.
2. **One OR-accumulated dirty word, tested once at `BEGIN`.** Ours is a single
   `s_vsh.dirty` flag. Theirs ORs a descriptor per method and tests three bits at
   `BEGIN`.
3. **Do no texture work at method time.** All 320 of their texture methods go to
   a bulk handler; everything is deferred.
4. **Queue fences, never wait inline.** Theirs is a 1,024-entry ring with a fatal
   trap on overflow.
5. **Thread `END→BEGIN(same primitive)`.** Worth more to us than to them, since
   `nv2a_metal.m` serialises on `raster_order_group(0)`.

**Why this is also the frame-time work:** D1 proved the drain is scene-driven,
not thermal, and we already hit 16.4–16.8 ms in a light scene. The deficit is
what a heavy scene does to the drain — which is the method path and the resolves.

**Measure each step separately.** `NO_DEPTH_SYNC` won 9.1% alone; that is the
bar, and a batch of five changes measured together tells us nothing.

### A2. Resolves — ask the question that already paid once

Their per-title switches grant permission to elide resolves
(`xoallowtitletoskipresolves`). We asked exactly that question of depth once and
`RECOMP_METAL_NO_DEPTH_SYNC` won 9.1%. Ask it of colour at surface swap. `src/`
work, one switch, one A/B.

### A3. G2 — the glyphs, with a corrected instrument and lowered expectations

**The FB-WATCH dump cannot be armed as-is** (measured: ~100 small-change events
per minute, uniformly, and each dump is a full 640×480 frame at 921,654 bytes, so
any cap is spent within 30 s on animation). Fix the trigger to what the defect
actually is: **changed while the scene was otherwise static**, not "changed a
little".

And lower the expectation. `VGPUDX12.dll` ships **36 hand-written per-title
shader overrides** with internal bug numbers against them. Seven hypotheses have
died here hunting a general mechanism. **A per-title hook is ordinary engineering
in this domain**, and is better designed deliberately than discovered at 2am.

### A4. G1 — the music, once §0 has forked it

Then, and only then, xemu's `vp.c` — voice-list splicing on VOICE_ON,
`voice_lock`, mixbin handling for 3D voices. Our APU is xemu's and not one of the
seven dead hypotheses was ever checked against it. No local copy; fetching it is
step one. The still-running MCPX agent may add a second independent reference.

---

## GOAL B — IMPROVE XBOXRECOMP

### B0. FIRST, the gate nobody has measured

`src/kernel/irq_latency.{c,h}` is **written but not wired** (18 Sep night).
Finish it or delete it — half-built instrumentation is worse than none.

It answers the question the whole interrupt argument rests on. The handover says
in its own words: *"NOT DEMONSTRATED ... the crash rate has not been measured
against a shorter window."* If raise→delivery is already microseconds, the poll
byte buys nothing and we save a regeneration. If it is milliseconds, the argument
is demonstrated rather than reasoned.

### B1. THE ONE REGENERATION — batch these, do it once

Do not spend a regeneration on any single item below.

1. **The poll byte** *(gated on B0)*. Microsoft emits a one-byte poll roughly
   every 79 guest bytes — at backward branches, at returns, and before every
   interrupt-disable escape — so guest code is interruptible everywhere and
   delivery never waits for a blocking call. Ours runs only when a guest thread
   calls a wait function, rate-limited to 8 ms.
   **It would subsume `RECOMP_IRQ_THREAD` rather than compete with it**: a poll
   site is a guest instruction boundary, so the ISR runs on the guest's own stack
   and needs no borrowed worker slice — which is precisely what blocked that
   experiment.
   Also separate "advance time" from "deliver interrupts"; we conflate them in
   one 8 ms period and they keep them strictly apart.
2. **Populate `g_flat_table` at basic-block granularity.** The structure already
   exists (`translator.py:2056`, byte-indexed, binary-search fallback, tested at
   499,688 bytes) and is populated from detected **function starts only**. The
   work is entries, not architecture. Microsoft's answer was every guest byte,
   with two thirds of those being short real translations that rejoin the
   instruction stream — not error paths.
3. **Dead-flag elision in the lifter.** They compute flags eagerly and *free*,
   and still elide them (822 cases in a 3 MB window). We pay for every flag in C.
   This is the highest-value pure-performance item and it is ours to take.
4. **`EFLAGS` materialised only at `pushfd`/`popfd`**, as they do.
5. **An opt-in trap instead of the stack-balancing `*_stubs_unresolved.c` body.**
   A stub that adjusts `esp` and returns is not an honest answer to "we could not
   prove this statically".

**Re-verify the baseline immediately after**, before any A/B is run against the
new gen. Every archived measurement predates it.

### B2. Boundary detection is not the deficiency we thought

Microsoft had the source, the symbols and a build farm and **did not solve x86
function-boundary detection either**. They gave every guest byte an entry point
and let the rest resync. Our 8,876 functions are not a defect to be engineered
away by a better detector — the fallback is the answer. Stop treating detection
as the gap.

### B3. Owed upstream *(cheap, unchanged)*

G15 (`bts`/`btr`/`btc` CF), G16 (narrow rotates), `get_data_ptr`'s dead bound as
an issue. PR #67 and #69 open, maintainer quiet since 16 Sep.

---

## WHAT NOT TO DO

**No more BC mining.** Five agents have worked the seams: symbols, codegen, the
address maps, the kernel, the NV2A translator, and the MCPX APU model in flight.
What remains is depth on questions we have not got, and the binding constraint
stops being availability and becomes the white-room boundary — which matters if
any of this is upstreamed. One agent decrypted a null-key XEX after I failed to
restate the limit in its brief; that artifact stays out of the repo.

**No more instruments before the ones we have are read.**
`RECOMP_APU_TRAP_THREADS`, `RECOMP_KERNEL_THREADS` and `RECOMP_GUEST_NAMES` are
armed and unread. The tree has a documented habit of building the next instrument
instead of reading the last one.

## THE ORDER, IN ONE LINE

Session (§0) → wire or delete the latency gate (B0) → NV2A descriptor table and
dirty word (A1, measured one at a time) → colour-resolve question (A2) → fix the
FB-WATCH trigger (A3) → one batched regeneration (B1) → re-verify baseline →
upstream debt.
