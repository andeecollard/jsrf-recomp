# JSRF goals — Track A, and a compiler that lies, 17 September 2026, night (2)

Supersedes `JSRF_GOALS_2026-09-17_NIGHT_FINISH_TRACK_A_FIRST.md`. The
**target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

**G-numbers are unchanged, again and deliberately.** Handovers, commits and
now a progress note cite G1b, G1d, G3 and G14 by name. Priority lives in
*The order*. G1–G13 carry their meaning forward; the evidence for G1–G5 is in
`JSRF_GOALS_2026-09-17_THE_GUEST_STOPS_TALKING.md` and is not re-pasted.

## What changed since the last list

Two things, and neither is a new theory about the music.

**The build is a variable in principle.** G14 — the SF condition is
signed-overflow UB that the compiler folds from `-O1` — means the optimisation
level can change *semantics*, not just speed. **In this image it does not**:
all five UB-form sites compare against zero, so the fold is a no-op here (see
G14). The rule stays because the next regeneration need not preserve that, and
because the general point holds for any UB the lifter emits.

**The ecosystem was surveyed for the first time.** Four things we owe upstream,
two things worth taking from them, and four dead ends closed in *R* — one of
which is this list's own previous entry for G3.

## Where we actually are

| | measured | source |
|---|---|---|
| frame median | **18.5 ms** against a 16.68 ms budget | scripted, 240 s, METAL_FF=1 |
| frame without sync | p50 **8.0** ms, p99 29.5 ms | `[NOSYNC]` |
| music | **dies ~2 min in, every session** | player, 17 Sep |
| text | corrupt in speech boxes AND trick names | player screenshots |
| fences, sound effects | fixed, player-confirmed | — |
| tests | 48/48 green | `ctest`, build-feav |
| lifter vs Unicorn | 860 mismatches / 13,008 vectors, 3 families | `fuzz_unicorn.py` |
| tree | `main` at `62eb616`, level with `origin/main` | — |

## The rules that order this list

**De-risked work outranks interesting work.** G3 was ranked first on the
morning of 17 Sep with a completed measurement saying GO. It was not started,
because every session produced a fresher theory; five of those died the same
day. This is the rule that cost a whole day.

**A new finding is not a reason to re-order.** Seven were found on the night of
17 Sep (G10–G16). None of them displaces G3. G14 is the hardest case — it is a
wrong branch in the code the player is running — and it still does not, for the
reason in its own entry.

**Measure reachability before urgency.** G14, G12 and G13 are all "the code is
wrong", which is not the same claim as "this is happening". Each carries a
counter, and the counter comes first.

## The order

0. **The player's next session**, with `RECOMP_APU_CYCLE_BREAK=1`. It is one
   line, it needs no build, and it is the first time there has been a reason to
   set it. The same session re-tests XC_AUDIO=stereo, which has never been
   heard, and can finally take G1's delivery measurement — scripted runs cannot,
   because they do not reach the state.
1. **G3's depth question** — why depth is dirty at nearly every swap, and
   whether guest RAM ever needs it. The A/B is DONE and says A2 is inert: one
   deferral per run against thousands of depth refusals. Do not re-run it.
2. **The near-free counters.** G14's reachability is **answered** (statically —
   all five UB-form sites compare against zero, so it cannot fire here). G10's
   thunk report and G11's `XC_AUDIO` probe are **built**; each needs one run to
   confirm the line appears. None changes runtime behaviour.
   **Not during the A/B, though.** They are all `src/` edits, and
   `play_scripted.sh:126` fails a run when any `*.c`, `*.h` or `*.m` under
   `src/` or `diagnostics/jsrf_first_fault/` is newer than the binary — so a
   counter written mid-A/B kills every remaining run in the loop. `docs/` and
   `tools/` are not scanned and are safe.
3. **The upstream debt** — four items, ranked at *What we owe upstream*.
4. **G14's fix**, gated on its counter. Expensive: it changes generated code
   for every `js` in the image, so it needs a regenerated gen and a re-verified
   baseline.
5. **G1**, by measurement only — never by another mechanism.

G2 and G4 stay parked, with reasons at their entries.

---

## G1 — Why the guest stops issuing APU methods *(open, method-gated)*

Two minutes in, every session, `guest_methods` freezes and the music dies.
Established and not in doubt: the guest is alive and still faulting on the
aperture (`[MCPX-TRAP] vp` equals `guest_methods` exactly, so nothing is lost);
`[APU-WRITE] main=16997 vp=11762` — it services our traps through the main
registers and submits no voice work; the trigger is a burst of VOICE_ON that
the player located ("the sound breaks up when you speak to gum"); and the
invariant is that **retired voices stay in the list and the guest never takes
them out** — eight guest writes to the 3D list head in a whole session against
thousands of raises.

**Done when:** we know why the guest acknowledges a removal request and does
not perform the removal.

**The next measurement, specified and still unbuilt:** the trap carries ONE
handle in FEDECPARAM. When several voices retire inside one burst, how many
distinct handles were ever *delivered* against how many went idle? The
`seen`/`delivered` pair exists and was re-anchored in `5e47836`; it needs a run.

- **G1a** `RECOMP_APU_FEDEC_HOLD` default — **DONE, but it does NOT fully fix
  the crash.** 18 Sep: the crash it was built for fired **three times in one
  A/B with the hold ON** — all at `sub_001A2E2E +0x670`, fault `0x70FFFFFFBE`,
  the known DSOUND signature, in runs reporting `fedec_hold on (default)` and
  `decode pairs held while trapped: 3`. The hold did its job on those methods
  and the title crashed anyway. So the decode-pair race is *a* cause, not *the*
  cause. G1a rested on one player session with `held=1702` and no crash, and
  its own entry called that a confirmation rather than a measurement — this is
  what the second session it asked for looks like.
- **G1b** why retired voices stay linked — open, and **reopened on the cycle
  hypothesis, 18 Sep**. The player's session:

      [APU-CYCLE] relink=90 (of 212 top inserts) walks_with_a_cycle=88
                  broken=0 last=v3/list1 (cycle_break OFF)
      [APU-WALKCAP] hit=87

  88 walks met a ring, the cap was hit 87 times, and the voice closing the last
  one is v3 — a storm voice. The refutation that closed this was taken from a
  run that never reached gameplay; a scripted run tonight reproduced that false
  zero exactly (4 top inserts against 212, gate 1 failed). **`cycle_break` has
  never been switched on**, so nothing is known about breaking the ring. That
  is the next thing to try and it costs one line in `paths.conf`.
  `RECOMP_APU_IDLE_TRAP_SELFLINK` remains refuted by its own counter (`0 of 0`)
  and must not be re-armed without a session showing `encounters` moving.
- **G1c** the music decays rather than cuts out — open. Not the APU falling
  behind. **Done when** we know what declines, per voice.
- **G1d** the storm precedes the collapse — open, one session. **Done when** a
  second reproduces the ordering.

## G2 — The glyph index error *(open, parked)*

One glyph's quad drawn with another's texture coordinates, inside one batch;
wider than the tutorial. **The label trap is dead** — it alternates ABABAB by
frame parity and fires every frame catching nothing — so re-arm against frame
**N−2**, or key on the surface address, before trusting any `watch*.bmp`.

Parked because it is cosmetic and rarer than the flicker. **Not** because A1
will fix the flicker: that claim was made in this afternoon's plan and
retracted by A1's own commit — the swap already syncs eagerly, so the
discarded range was never what made the label alternate. See *R*.

**Done when:** the player sees clean text across a session, and a scripted
pixel diff of a text frame between CPU and GPU arms is empty.

### THE TEXTURE MATRIX CONVENTION IS UNMEASURED, AND IT IS TEXCOORD-SHAPED

Found 18 Sep by following the only unclean render number in the player's
session (8 of 608,249 VSH batches rejected for "fixed-function clip W", with
`METAL_FF=1`). The rejection itself is small and known — `nv2a_ff.c:32` records
1–193 such batches per run — but the comment beside it names an open question
that nobody has closed, and it is about **texture coordinates**:

> For the COMPOSITE matrix the question is already answered by the picture […]
> For the TEXTURE matrix nothing has measured which way round the driver
> uploads it, and the consequence is not symmetric: a last-COLUMN read gives
> `q = q_in`, a constant; a last-ROW read gives
> `q = tx*s + ty*t + tr*r + tq*q_in`, which is **data-dependent and can reach
> zero for some vertices and not others**.

Data-dependent, per-vertex, sparse texcoord error is the exact shape of "one
glyph's quad drawn with another glyph's texture coordinates". The tree already
measured the residue that fits it — `texcoord-q = 3434 of 172,815,065
triangles, sparse, stops growing once the scene settles`.

**Everything needed to settle it already exists and has never been run.**
`RECOMP_FF_DUMP` prints texture matrix 0, and the comment states the decision
rule outright: *a last row of `(0,0,0,1)` means `matrix()` is right; a last
COLUMN of `(0,0,0,1)` means the transpose is.* The transpose is already
available as a switch.

**Checked, not assumed:** no `[FF] texture matrix 0` dump appears in any
`stderr.log` in `render-investigation/`. The runs that do exist report
`texture matrix: 0 never uploaded, 0 all-zero`, which says the path was not
exercised in those scenes rather than that the convention is right.

**RUN 18 Sep, AND IT KILLS THIS LEAD.** `RECOMP_FF_DUMP=1`, scripted, 200 s,
reached scene 30 and held it 131.7 s:

    [FF] unit 0: texgen s/t/r/q = 0000 0000 0000 0000, TEXTURE_MATRIX_ENABLE=0
    [FF] texmat0 0x6C0 (seen first=0 last=0): [0 0 0 0] [0 0 0 0] ...
    [FF] unit 1: ... TEXTURE_MATRIX_ENABLE=0
    [FF] unit 2: ... TEXTURE_MATRIX_ENABLE=0

**JSRF never enables a texture matrix, on any unit, and never uploads one.**
The convention question `nv2a_ff.c:38` leaves open is real and still unanswered
for other titles — and it cannot affect this one. Dead.

*(First attempt produced nothing because I ran it with `RECOMP_METAL_FF=1`,
which routes fixed function to the GPU and bypasses the CPU path that does the
dumping. The dump lives in `nv2a_ff.c`, the CPU implementation.)*

**What the same dump did establish**, since it is the first time anyone has
read it:

- `clip/w = [768.14, 150.33, …]` against the printed rule — hundreds, not ~1 —
  so **VIEWPORT_SCALE is baked into the composite matrix**. That is question 1
  of the two the comment poses, answered.
- `texgen s/t/r/q = 0000` on every unit: no texture-coordinate generation.
- `lighting 0x0314=0 … (seen=0)`: the lighting registers are never written.

So this title's fixed-function path is a composite transform and nothing else.
That narrows what FF could be getting wrong for glyphs to the composite
transform and the vertex attribute fetch — texgen, texture matrices and
lighting are all provably not in play.

**New, and worth watching rather than acting on:** phobos665's fork is hitting
a text defect too — *"HUD text draws as solid blocks instead of glyphs"*,
unresolved, suspected `d3dcolor_to_float4` ARGB constant layout. Different
symptom, same hardware. Their combiner alpha-channel bug is **not** ours;
checked both backends (`nv2a_d3d11.c:174`, `nv2a_metal.m:647`).

## G3 — The frame tail *(BUILT and MEASURED. A2 is inert; the blocker is depth.)*

**CORRECTED, 17 Sep night. A1 and A2 are already committed and tested.** Two
earlier documents — this afternoon's plan and the 19:48 goals file — both say
"build `nv2a_metal_sync_range` first". That work landed at 18:35 the same day
and both were stale when written. The commits are the authority:

    2b5bdb2  G3 A1: the flip's range is honoured instead of discarded
    ff07677  G3 A2: mark the debt instead of paying it at every surface swap

`nv2a_metal_sync_range` exists at `nv2a_metal.m:2207`, is declared in
`nv2a_metal.h:17`, and `nv2a_pb_exec.c:21` points the macro at it. Three tests
are registered and green in the 48: `jsrf_metal_sync_range`,
`jsrf_metal_defer_swap_off`, `jsrf_metal_defer_swap_on`. `ab_score.py:162`
already maps `RECOMP_METAL_DEFER_SWAP` to the token `defer_swap`, so the A/B
harness is wired too.

**A1 changes nothing on its own, deliberately.** Every draw sets
`surface_dirty` and every swap syncs before it rebinds, so no slot owes
anything for rendered content and the range walk finds nothing to pay.
`paid=0` across a run is the expected reading, and is also the positive
control that it walked rather than never ran. A1 is what makes A2 *safe*, not
a fix in itself.

**A2 is `RECOMP_METAL_DEFER_SWAP`, default OFF.** It marks the outgoing slot
as owing guest RAM and clears `surface_dirty`, so the sync takes its clean
early return: no drain, no read-back. It refuses rather than guesses — if the
outgoing surface is in no slot, or if depth is dirty — and counts both
refusals.

### MEASURED 17 Sep night, and A2 is inert. The blocker is DEPTH.

`ab_switch.sh`, three trials per arm, 240 s, binary pinned, ABBA:

    =0   25.40  18.42  19.40 ms   (mean 21.07, n=3)
    =1   39.67  20.47        ms   (mean 30.07, n=2; t1_defer1 excluded, scene=12)
    THE RANGES OVERLAP -- 9.00 ms of difference inside the run-to-run spread.

**The frame times say nothing because the switch does nothing.** Per run:

    deferred: 1   refused: 5747 / 7295 / 12656 depth dirty,  0 no slot

One deferral per run and thousands of refusals, all of them for depth.
`nv2a_metal.m:3642` is `if(depth_dirty){++g_swap_defer_depth;}` — A2 refuses
whenever depth is dirty, because `surface_slot_writeback` carries colour only.
In a mission depth is dirty at essentially every swap, so the swap keeps paying
its full drain and read-back. The A/B compared not-deferring with
not-deferring.

**So the question changes.** Not "does deferring help" — the path is live and
the one deferral per run proves it. Ask instead:

1. Why is depth dirty at nearly every swap in a mission?
2. **Does guest RAM ever need that depth?** A slot's depth is retained on the
   GPU for the rebind, and the refusal exists only because the writeback cannot
   carry it. If nothing reads depth from guest RAM, the refusal is protecting a
   value nobody consumes. `no slot` refusals are 0, so the other path is idle.
3. If something does read it, can depth be written back on the same range-aware
   terms A1 already established for colour?

**Do not re-run this A/B first.** Six more runs would re-measure the same inert
switch.

### WHERE SYNC'S TIME ACTUALLY GOES, measured 18 Sep

From a completed gameplay run (`t2_defer0`, 160 s, scene 30):

    [METAL] sync 38089 calls (12696 already clean):
            83847.0 ms draining the GPU, 22728.8 ms reading back and converting
    [METAL] sync callers: 25372 surface swap, 0 invalidate, 0 frame end,
            12717 external

**The drain is 79% of it, not the read-back.** The plan this goal inherited was
written as though the 4.9 MB copy were the cost; it is the *wait*. Deferring
removes both, so the conclusion is unchanged — but anyone optimising the copy
alone would have been optimising the smaller fifth.

Also worth keeping: resident clears already handle 16,851 of 16,863 colour and
25,375 of 25,397 depth/stencil clears, so that optimisation is done and is not
where the remaining time is.

### `RECOMP_METAL_NO_DEPTH_SYNC`, built 18 Sep, default OFF

The cheap way to ask whether guest RAM needs the depth at all. Diagnostic in
the same sense as `RECOMP_METAL_HW_NO_STENCIL`: a path that never returns depth
is wrong by construction *if anything reads it*, and the point is to find out
whether anything does. Counted in both arms, so a run with it **off** still
reports how many write-backs it would have skipped — which is what decides
whether the A/B is worth taking.

Registered with `ab_score.py` as `no_depth_sync`, and the generic
`METAL_SWITCH_RE` can see it, so its VOID check will actually run.

### RUN 18 Sep. The switch works; the frame-time half is VOID.

    =0   20.56 ms                  (n=1 -- two runs excluded, one crashed)
    =1   19.05  19.29  19.65 ms    (n=3, mean 19.33)
    VOID: ONE RUN PER ARM IS NOT A MEASUREMENT (n=1 vs 3)

The scorer is right to void it and the exclusions are not the switch's doing —
both excluded runs are the **control** arm failing to reach a mission
(`scene=12`), which is boot flakiness this host has shown before.

**What is NOT void is the mechanism**, and it is the point of the switch:

    =0   depth write-backs: 25512 taken,     0 skipped  (no_depth_sync OFF)
    =1   depth write-backs:     0 taken, 27177 skipped  (no_depth_sync on)
    =1   depth write-backs:     0 taken, 27143 skipped
    =1   depth write-backs:     0 taken, 26611 skipped

**~27,000 depth write-backs per 160 s run were skipped entirely, and all three
runs reached scene 30, held it 159 s, and finished without a crash.** Each
skipped write-back is a drain as well as a copy.

Rendering survived it, by the checks that exist:

    [d3d8_gl] blit 13500: expected 41 69 53 -> window 41 69 53  delta +0 +0 +0  MATCH
    [FB] t=239.00 nonzero=153362/153600

**The honest limit:** the blit check is ONE pixel and `[FB]` is a sum. Neither
can see a depth-dependent artifact in the middle of the frame. "It ran and
presented" is not "it rendered correctly", and this is exactly the distinction
this tree has been burned on before.

### THE REPEAT SEPARATES. First A/B in this phase that does.

    RECOMP_METAL_NO_DEPTH_SYNC=0   20.44 - 21.35 ms  (n=2)
    RECOMP_METAL_NO_DEPTH_SYNC=1   18.85 - 19.15 ms  (n=2)
    ON is faster and the ranges DO NOT OVERLAP (9.1% less frame time)
    arms verified distinct: =0 reported "no_depth_sync OFF", =1 "no_depth_sync on"

Across both A/Bs, eight usable runs, and **every `=1` value is below every
`=0` value**:

    =0   20.56  20.44  21.35                 (n=3)
    =1   19.05  19.29  19.65  19.15  18.85   (n=5)

`max(=1) = 19.65 < min(=0) = 20.44`. The arms verified distinct this time
because the generic `METAL_SWITCH_RE` fixed yesterday can see the token — the
defer_swap A/B could not, and ran with its VOID check skipped.

**Say what it does not buy.** 19 ms against a 16.68 ms budget. This is ~9%, it
is real, and it is **not 60 fps on its own**. `[NOSYNC] p50 = 8.0 ms` remains
the upper bound and nothing yet reaches it.

**Correctness, and its limit.** Zero non-MATCH blit checks across all twelve
runs in both arms. But that is ~30 sampled checks of ONE pixel per run: it
cannot see a depth-dependent artifact in the middle of a frame. A scene-matched
full-frame diff is still owed, and until it exists this is evidence that guest
RAM probably does not need the depth, not proof.

### THE NARROWING WORKS. THE COMBINATION DOES NOT HELP. *(18 Sep)*

A1 landed and did what it was built to do — deferrals per run went from **one**
to about **twelve thousand**, and the depth refusal to zero:

    [METAL] swap writeback deferred: 11874 (of which 11873 only because depth
            is not written back at all) (refused: 0 depth dirty, 0 no slot)

`jsrf_metal_defer_depth_refuses` / `_narrowed` both assert — verified with
`ctest -V` that neither *skipped*.

**And the frame time did not improve. If anything the reverse:**

    NO_DEPTH_SYNC=1 both arms, DEFER_SWAP=0   19.35 ms          (n=1)
    NO_DEPTH_SYNC=1 both arms, DEFER_SWAP=1   21.06 20.68 20.75 (n=3, mean 20.83)
    VOID: n=1 vs 3 — four of eight runs unusable

**Provisional and staying that way until a valid A/B says otherwise.** But the
direction contradicts A2's premise and is worth recording: deferring may not
remove the swap's cost so much as **move it to the flip**, where
`nv2a_metal_sync_range` walks the slot list and pays every overlapping debt at
once — plausibly worse than paying it spread across swaps.

### SETTLED, 18 Sep: `defer_swap` MAKES IT SLOWER. A2's premise is refuted.

Six trials per arm, `no_depth_sync=1` in both, arms verified distinct:

    DEFER_SWAP=0   19.08 18.90              (mean 18.99, n=2)
    DEFER_SWAP=1   20.74 20.88 20.62 20.53  (mean 20.69, n=4)
    OFF is faster and the ranges DO NOT OVERLAP (8.2% less frame time)

The earlier void A/B pointed the same way (19.35 off, 20.83 on), so two
independent runs agree on the direction and this one separates.

**So A2 is not merely inert — once it can fire, it is actively harmful.** The
premise Track A inherited, that the swap's write-back is waste that can be
deferred for free, is measured false. Deferring does not remove the cost; it
**moves it to the flip**, where `nv2a_metal_sync_range` walks the slot list and
pays every overlapping debt in one burst instead of spread across swaps.

**The whole of the win is `no_depth_sync`**, which stands on its own at 9.1%
across eight runs with non-overlapping ranges.

**`RECOMP_METAL_DEFER_SWAP` should stay off, and the A1 narrowing should stay
anyway** — it costs nothing with the switch off, it is tested, and it is what
made this measurable at all. Without it the switch could not fire and this
would still read as "inert".

**Exclusions, because the rate matters:** six of twelve runs never reached a
mission. That is the second A/B in a row losing half its runs, and it is now a
harness problem in its own right rather than bad luck.

**Do not ship on this.** `no_depth_sync` still owes its full-frame diff.

**Done when:** that question is answered, and — only if a change makes the
deferral actually fire — median frame under 16.68 ms in a mission, verified
with the `[d3d8_gl]` blit check, with the player having seen a clean frame.
A frame-time win with corrupted pixels is not a win.

**One run to keep in view, not to act on:** `t1_defer1` stopped at scene=12
with `flips=0`. One occurrence, excluded by the harness by design, and the
other two `=1` runs reached scene 30. Boot safety is not measured here.

Full account: `docs/jsrf/progress/PROGRESS_2026-09-17_NIGHT_THE_DEFER_SWAP_AB.md`.

**Say the caveat before measuring:** `[SYNC]` p99 is 16.0 ms and `[NOSYNC]`
p99 is 29.5 ms. This buys the **median**, not the hitches.

## G4 — The `RECOMP_SYNC_HIST` halt *(open, OFF, parked)*

1 halt in 4 runs on, 0 in 4 off; the 17 Sep A/B did not reproduce it across
three trials per arm. **Done when** there is an explanation or an n large
enough to exonerate it. Neither is worth buying yet.

## G5 — Delete or re-word `[APU-POOL] on_2d` *(open, trivial)*

It counts the guest *starting* a 2D voice, so it flatlines during healthy
playback, silence and mid-flight death alike — it cannot separate the three
things it was added to separate. **Done when** it is gone, or its line says so.

---

## G10 — The thunk table's report is false *(new 17 Sep)*

`143/378 resolved, 235 unresolved — game may crash!` is wrong twice. JSRF
imports **120** ordinals, **four** unresolved (91 `IoDismountVolumeByName`,
144 `KeSetDisableBoostThread`, 204 `NtProtectVirtualMemory`,
232 `NtUserIoApcDispatcher`). Past slot 120 the loop falls into another title's
fallback list for 27 slots, then reads past its 147 initialisers and logs
`Unresolved kernel ordinal 0` 231 times. `116 + 27 = 143`, `4 + 231 = 235`.

Cost is entirely diagnostic — nothing reads the table outside
`kernel_thunks.c`, the unresolved handler has been called **0** times, and the
bridge implements three of the four.

**DONE 17 Sep 22:30.** The loop picks its source once instead of per slot and
stops when the mapped table is exhausted:

    Thunk table: 116/120 resolved, 4 unresolved (from the mapped XBE)
    WARN  4 kernel import(s) unresolved: ordinal 204, 232, 144, 91. The bridge
          may still implement them -- this table is only reached if the guest
          calls through the XBE's thunks.

Exactly the predicted arithmetic. The 231 `Unresolved kernel ordinal 0` lines
are gone and so is the false "game may crash!". **Still owed:** a test that a
short mapped table does not borrow the fallback list.

**A second reason to do it:** that dead table is the *only* referent keeping
`kernel_memory.c`'s contiguous-memory functions alive, and those contain the
alloc/free mismatch upstream found in PR #60 (see *R*).

## G11 — `XC_AUDIO` tells the title the console is mono *(new, hypothesis, gated)*

We answer index 0x09 with `0x00010001` — the channel field is `0=stereo,
1=mono, 2=surround`, so that is **mono with AC3 advertised**, an encoded path
we do not have. The comment beside it claims the low bit means stereo and is
wrong. `upstream/main 8a78867` changed this exact constant to `0x00000000`
independently. `kernel.h:1032` records this tree already being burned once by
this value.

**Its standing rose on 17 Sep, from the dsound survey.** JSRF's DirectSound is
the title's own XDK code recompiled from the XBE — nobody in the ecosystem can
improve it, because nobody has it. The only levers are the values our APU model
and kernel *present* to it, and this is one of the very few that is read before
it decides anything.

**Still gated, and the gate is the point.** It is not established that JSRF
reads 0x09 at all — the log line is `XBOX_LOG_DEBUG` and player runs are INFO.

### STEP 1 IS DONE, 17 Sep 22:30, AND THE TITLE DOES ASK.

A 70 s scripted run, with a new one-line-per-index `[EEPROM]` probe at INFO:

    [EEPROM] index 0x011 queried (first time), len=4, answered 0x00000000
    [EEPROM] index 0x00A queried (first time), len=4, answered 0x00000000
    [EEPROM] index 0x103 queried (first time), len=4, answered 0x00000000
    [EEPROM] index 0x008 queried (first time), len=4, answered 0x00080000
    [EEPROM] index 0x009 queried (first time), len=4, answered 0x00010001
             <- XC_AUDIO: 0=stereo 1=mono 2=surround, bit16=AC3

**JSRF reads `XC_AUDIO` at start-up, before any voice is submitted, and we
answer mono-with-AC3.** Step 1 was written as the measurement that could kill
this hypothesis in one run. It did not kill it.

Also caught: index **0x103** (`XC_FACTORY_AV_REGION`) is queried and falls
through to the `default:` arm, which returns `STATUS_SUCCESS` with a zeroed
value. A second unhandled start-up answer nobody had looked at.

### STEP 2 SHIPPED 17 Sep 22:40, AND THE LISTEN IS OWED.

Default is now `0x00000000`, stereo PCM. `RECOMP_EEPROM_AUDIO_LEGACY=1`
restores `0x00010001` exactly, through `recomp_switch_on` — the ratchet refused
the first version, which hand-rolled its getenv, and was right to.

Three test arms, and the DEFAULT one is the one that matters, because the
defect was a default: `jsrf_eeprom_audio_default`, `..._legacy`, and
`..._legacy_word_grammar`, the last passing `yes` so it also pins the grammar
the hand-rolled form gets wrong. 51/51. **Verified by injected fault:**
restoring the old constant fails the default arm alone, with
`XC_AUDIO answered 0x00010001, expected 0x00000000`.

**Verified end to end in the shipped bundle**, not just in the test:

    [EEPROM] index 0x009 queried (first time), len=4, answered 0x00000000

**NOT YET HEARD, 18 Sep.** The player reported "sound still messed up" on a
session launched at 22:40:02 against an engine written at 22:40:24 — twenty-two
seconds too early. Their log proves it: `index 0x009 ... answered 0x00010001`,
the old word. The verdict is void and the flip is still untested by ear.

**Done when:** the player has listened. Because the switch is read through
`paths.conf`, the A/B costs them an `export` and no rebuild:

    export RECOMP_EEPROM_AUDIO_LEGACY=1   # the old mono+AC3 answer

**What a result looks like, stated before the listen so it cannot be fitted
afterwards.** The music still dying ~2 min in refutes this as the cause of G1;
it does not refute the constant being wrong, which is settled on the encoding.
A change in the balance, the positioning or the survival of the music is the
thing to report either way.

## G12 — `MmGetPhysicalAddress` returns a virtual address *(count first)*

`kernel_bridge.c:2208` returns the guest VA unchanged; upstream `127f3fa` folds
the contiguous arena. We use `XBOX_CONTIG_BASE` in five other places in the
same file. **JSRF imports ordinal 173**, confirmed against the XBE. Whether it
ever passes a contiguous address is unasked, and `xbox_memory_layout.c:3286`
already does the inverse fold, so a compensating mask is plausible.

**Done when:** a counter has bucketed the argument by window. Count before
enforcing.

## G13 — `fldcw` is recorded and never read *(count first)*

The lifter models `fnstcw`/`fldcw` into `g_fp_control_word`; no FIST helper
reads it. **The tree's nearest-even argument is sound and must not be undone**
— `_ftol2` does not reprogram the word, across 432 call sites. But the image
has exactly two `fldcw` sites, both in `sub_0017F03A`, the `_control87`
pattern, so "nothing changes RC" is an assumption with a cheap test and no test
behind it.

**Done when:** a counter on writes with `(cw >> 10) & 3` nonzero has run. If it
never fires, record that in `recomp_types.h` and close the question.

---

## G14 — The SF condition is signed-overflow UB *(real defect, NOT reachable here)*

**CORRECTED 17 Sep night. It is not live in the player's build, and my earlier
entry saying it was is withdrawn.**

The defect is real. `js`/`sets`/`cmovs` after a `cmp` can emit
`if (((int32_t)((_fas) - (_fbs)) < 0))`; signed overflow is UB, so from `-O1`
the compiler folds it to `_fas < _fbs`, which is a different function whenever
the subtraction overflows. Demonstrated on the exact expression with
`0x80000000` and `1`, where x86 gives SF=0: `-O0` answers 0, `-O1` and `-O2`
answer 1.

**What I got wrong: I counted one expression form and called it the site
count.** JSRF has **701** SF consumers, not 5 — 298 `js`, 403 `jns`, one
`sets`. Enumerated by form:

    252  TEST_S(_fas, _fbs)                       test/AND -- cannot overflow
    317  ((int8_t|int32_t)((_fa) & (_fb)) >= 0)   test/AND -- cannot overflow
     54  (_fas >= 0) / (_fas < 0)                 sign of one value -- safe
    ~73  ((int32_t)(REG|MEM) < 0)                 sign of a wrapped value -- safe
      5  ((int32_t)((_fas) - (_fbs)) < 0)         THE UB FORM

`TEST_S` is `RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), width) < 0` — an AND,
so there is no subtraction to overflow.

**And all five of the UB-form sites compare against a literal zero:**

    _fa = (uint32_t)(MEM32(ebp)) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    /* cmp MEM32(ebp), 0 (32-bit) */

`_fas - 0` cannot overflow for any `_fas`, so at every site in this image the
UB expression is exactly equivalent to the correct one. **No run was needed and
no counter was built** — the reachability question is answered statically, by
reading the operands I should have read the first time.

**Still worth fixing, and still worth sending.** The emission is wrong, the fix
is one expression — `((uint32_t)_fa - (uint32_t)_fb) >> 31` — and relying on
"every site happens to compare against zero" is relying on a property of one
title's code that a regeneration or a different title can remove without
warning. It belongs with G15 and G16 in the upstream debt, not in the
player-facing queue.

`jl`/`jge`/`jle`/`jg` are **not** affected: SF≠OF is mathematically signed
less-than. Only `s`, `ns`, `o`, `no`.

**Done when:** SF is the sign bit of the wrapped result, with a regression test
pinning the `0x80000000 / 1` vector **at -O2**. The runtime count that used to
be required here is no longer owed.

## G15 — `bts`/`btr`/`btc` report CF after their own write *(latent here)*

CF is reconstructed at the consumer by re-reading the bit (`lifter.py:872`),
but the instruction already modified it — always 1 after `bts`, 0 after `btr`.
The test-and-set idiom reading its own answer. Plain `bt` is fine.
**Zero occurrences in JSRF**, counted. Upstream's to have.

**Done when:** the pre-state is snapshotted at the instruction.

## G16 — Narrow rotates rotate at 32 bits *(latent here)*

`SET_LO8(eax, ROR32(LO8(eax), 2))` rotates a zero-extended byte inside a 32-bit
word and discards the wrap-around; the count is also not reduced modulo the
operand width. Same class as the `sar` width bug this tree fixed and sent as
PR #57 — the rotates were missed then. **Zero narrow rotates in JSRF**,
counted. Upstream's to have.

**Done when:** `ROL8/ROR8/ROL16/ROR16` exist with the count taken mod width.

## Carried forward, unchanged and untouched

- **G6** the ADPCM cause (the guard makes it silent, not correct)
- **G7** the page-table bound that cannot fire; count before enforcing
- **G8** the PCM stereo page straddle (latent: failing voices are mono)
- **G9** 128 switches still hand-roll their grammar; migrate at leisure

---

## What we owe upstream — four items, one sent

Ranked. `origin` is not a fork; PR #57 is merged, so this is an established
path.

1. ~~**The mixbin discard.**~~ **SENT 17 Sep 2026 night as
   [PR #67](https://github.com/sp00nznet/xboxrecomp/pull/67)**, from
   `andeecollard:mixbin-mixdown`. `upstream/main:src/apu/apu_dsp.c:150` reads
   bins 0 and 1 and throws away 2–31, so every 3D voice — every sound effect in
   any title — is computed and discarded. Ported as `RECOMP_APU_MIXDOWN_ALL`,
   default on, with `tests/apu_mixdown` registered twice and **verified by
   injected fault**: reverted to upstream's version, `apu_mixdown_all` fails and
   `apu_mixdown_two_bins` passes. Built and run on macOS ARM64.
2. **G15**, `bts`/`btr`/`btc` CF. Found by the fuzzer, latent for us.
3. **G16**, narrow rotates. Same, and it completes PR #57's story.
4. **`get_data_ptr`'s dead bound** (our G7), as an issue rather than a patch —
   both call sites pass `0xFFFFFFFF`, so a read past the page table returns a
   translation built from whatever dword sits there.

`~/jsrf-build/upstream-contrib` is finished work on a merged branch and is the
place to raise these from.

## What is worth taking from them

- **dplewis PR #59, conformance on Apple Silicon.** Two Docker images, build on
  amd64 and execute on i386, because Rosetta cannot run 32-bit x86 at all. This
  would give the fuzzer a **native oracle** instead of Unicorn, which upgrades
  the evidence behind G14–G16 from "two models disagree" to "the hardware says".
- **phobos665's D3D8 frame capture and standalone replay tool.** G2's blocker
  is that twelve scripted runs never caught the glyph defect and one human
  session caught it repeatedly. Capture once, replay offline forever. Their
  layer is HLE-D3D8 and ours is LLE-pushbuffer, but `nv2a_pb_replay.c` already
  exists, so it is the capture format and tooling to read, not the code.

**No upstream merge.** 14 conflicts including `lifter.py` and `translator.py`;
merging those invalidates every archived gen. G11 and G12 are two cherry-picks
if their counters justify them.

---

## R — Refuted. Do not re-derive these.

| Idea | How it died |
|---|---|
| The flip sync is the cost | Per-caller counters: the flip's sync is already clean, 9,490 of 9,504. The surface swap is the cost. |
| Recovering deferred vblanks is worth ~4% | Guest ISR ran at 101 Hz against 59.94; re-delivering manufactures a tick. |
| ADPCM stride from the segment descriptor | A/B printed the counter in neither arm; the scene has no streaming ADPCM voice. |
| `samples_per_block > 1` breaking the block index | `oversize=0` in every archived run. |
| Overlapping ring reservations from a second thread | 0 draws from another thread. |
| Vertex data changing under the glyph batches | Zero changes across every font-page batch. |
| q ≤ 0 texcoords drawn where the CPU drops them | 0 in 445,498 batches, with a test proving the counter can fire. |
| The music death is dropped idle-trap interrupts | `[IRQ-VEC]` shows v1, v3, v5, v6 delivering steadily PAST the death. |
| `g_vector_in_service` leaked | Same evidence. Nothing is stuck in service. |
| Trapping the front end idles the sound engine | `trapped_skipped=0`, and `se = total − halted` exactly. |
| ~~The v1/v3 storm is a two-voice list cycle~~ **RETRACTED 18 Sep — see G1b** | Refuted on `walks_with_a_cycle=0, hit=0` from a scripted run that **never reached gameplay**, so the rings had no chance to form. The player's own session reads `walks_with_a_cycle=88`, `[APU-WALKCAP] hit=87`, `last=v3/list1`, and 32 idle-trap entries flagged `[C]` on v3. A zero without a positive control is not an absence measurement. |
| The music "slowing" is the APU falling behind | `[APU-FRAME]` steady ~7,500 frames/window across the exact windows where `2D heard` decayed to 0. |
| The self-linked head is the mechanism | `selflink_raises_withheld=0 of 0`. The switch engaged and never met one. |
| ~~The idle trap triggers the guest freeze~~ RETRACTED — see G1d | Refuted on a session where the storm was already running before the window examined. The 09:43 session has a clean baseline and reverses it. |
| 235 kernel imports are unresolved and the game may crash | 231 of 235 are empty slots the loop invented; 4 are real and 3 are implemented in the bridge. The handler has been called 0 times. See G10. |
| **Upstream's DirectSound work could help the music** | **New, 17 Sep. Upstream has exactly one dsound fix ever (`2d2d5e4`, cursor sync) and we already have it — it predates our merge base. And it could not matter: `src/audio/dsound_device.c` is NOT LINKED into `jsrf_first_fault` (checked with `nm`; no `DirectSound*` symbol present). JSRF's DirectSound is the title's own XDK code. The other ecosystem audio branches are Media Foundation and XAudio2; our backend is SDL2.** |
| **Upstream's contiguous-memory heap bug (PR #60) is live here** | **New, 17 Sep. Real upstream — POSIX `VirtualQuery` hardcodes `AllocationBase=NULL`, so `MmFreeContiguousMemory` always `free()`s an mmap'd pointer — and we have the identical code. But our bridge routes ordinals 165/171 to `xbox_ContiguousAlloc`/`xbox_HeapFree` (guest arena, canonicalised, instrumented). The broken pair is referenced only by the dead thunk table. Latent, not live.** |
| **The texture matrix convention explains the glyph defect** | **New, 18 Sep, and it was my own lead from four hours earlier. `RECOMP_FF_DUMP` run for the first time: `TEXTURE_MATRIX_ENABLE=0` on all three units and every matrix all-zero and never seen. JSRF does not use a texture matrix, so which way round it would be uploaded cannot matter here.** |
| **The discarded flip range is what makes the label flicker** | **New, 17 Sep. Retracted by A1's own commit (`2b5bdb2`) after the afternoon plan asserted it: every draw sets `surface_dirty` and every swap syncs before it rebinds, so guest RAM is already current for every surface and the range walk finds nothing to pay. A1 is infrastructure for A2, not a fix the player can see.** |
| **Track A is unstarted and A1 must be built** | **New, 17 Sep night. Said by the afternoon plan AND by the 19:48 goals file, and false in both: A1 and A2 landed at 18:35 as `2b5bdb2` and `ff07677`, with three registered tests. Two documents agreeing does not outrank the commit log.** |
| **G14's SF bug is live in the player's build** | **New, 17 Sep night, and it was MY claim two hours earlier. The UB form appears at 5 sites and all 5 read `cmp <mem>, 0`, where the subtraction cannot overflow. The other 696 SF consumers use `TEST_S` or a sign-of-one-value form, neither of which subtracts. I counted one expression form, called it the site count, and never read the operands. The defect is real; its reachability here is zero.** |
| **Deferring the surface swap is worth the median** | **RESOLVED 18 Sep, and the answer is no. With the depth refusal narrowed so it can actually fire, `defer_swap` is 8.2% SLOWER with non-overlapping ranges at n=2 vs 4, arms verified distinct, and a second A/B agrees on the direction. Deferring moves the write-back to the flip, where sync_range pays every overlapping debt in one burst. The win belongs to `no_depth_sync` alone.** |
| **Deferring the surface swap buys the median** | **New, 17 Sep night. Measured and not separated -- but the reason is that `RECOMP_METAL_DEFER_SWAP` deferred ONE swap per run and refused 5,747-12,656 on `depth_dirty`. The switch is inert in a mission, so the A/B compared not-deferring with not-deferring. `[NOSYNC] p50 = 8.0 ms` remains the upper bound; nothing has yet been built that reaches it.** |
| **A `setcc` after BT/BTS/BTR/BTC tells you something about the lifter** | **New, 17 Sep. Those instructions define CF only; OF, SF, ZF, AF and PF are architecturally UNDEFINED (SDM Vol 2A). 9 of the fuzzer's first 24 "mismatches" were two models' choices of undefined. The generator no longer emits them.** |

## Rules for this phase

- **The optimisation level is a semantic variable, not just a speed knob.**
  G14 makes `-O0` and `-O2` different programs. Scene-match *and* `-O`-match
  every comparison.
- **The commit log outranks any prose written earlier the same day.** Two
  documents said to build something that had been committed three hours before
  either was written, and a third inherited it. Handovers supersede handovers;
  `git log` supersedes all of them. Check the code before writing a goal that
  says "build X".
- **De-risked work outranks interesting work**, and a new finding is not a
  reason to re-order.
- **Measure reachability before urgency.** "The code is wrong" is not "this is
  happening".
- **Take the refuting measurement before building the theory.** Three
  hypotheses died on 17 Sep inside an hour, each to a counter already in the log.
- **No audio fix ships without a counter that can refute it** *before* the
  player is asked to test it.
- **An oracle is not silicon.** Unicorn, xemu and a second model are leads; the
  SDM and the hardware are verdicts. Adjudicate before believing.
- A player-facing default needs a picture or a listen, not a residual.
- One run per arm is a lean, not a measurement.
- Check `[APU-VOICE] on=` before scoring an arm: 148–453 is gameplay, 4–12 is
  the attract loop.
- For anything needing two minutes of real play, **the player's session is the
  instrument** — twelve scripted runs missed what one human session caught.
- `pgrep -x jsrf_first_fault` before touching `src/`.
