# JSRF goals — finish Track A first, 17 September 2026, night

Supersedes `JSRF_GOALS_2026-09-17_THE_GUEST_STOPS_TALKING.md`, which holds the
full evidence for G1–G5 and should be read for any of it. **The target is
unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

**G-numbers are deliberately unchanged from the superseded file.** The 17 Sep
handovers and commit messages cross-reference G1b, G1d and G3 by name, and
renumbering them to express a priority would break every one of those
references. Priority is stated separately, in *The order*, below. New findings
are G10–G13.

## Where we actually are

| | measured | source |
|---|---|---|
| frame mean | 16.79 ms (59.6 fps) | scripted, 240 s, METAL_FF=1 |
| frame median | **18.5 ms** — above the 16.68 ms budget | same |
| frame p99 | 40.0 ms, max 219 ms | same |
| sync, per frame | p50 **9.0** ms, p99 16.0 ms | `[SYNC]` |
| frame without sync | p50 **8.0** ms, p99 **29.5** ms | `[NOSYNC]` |
| fences | fixed, player-confirmed | — |
| sound effects | audible, player-confirmed | — |
| music | **dies ~2 min in, every session** | player, 17 Sep |
| text | **corrupt in speech boxes AND trick names** | player screenshots |
| tests | 48/48 green in `build-feav` | `ctest`, 17 Sep night |
| tree | `main` at `74d0ae2`, level with `origin/main` | 17 Sep night |

## The rule that orders this list

Written on 17 Sep and earned twice in one day, so it goes above the goals
rather than below them:

**De-risked work outranks interesting work.** G3 was ranked first on the
morning of 17 Sep with a completed measurement saying GO. It needed no player,
no new theory and no session time. It was not started, because every play
session produced a fresher audio theory. Five of those theories died the same
day, four of them to counters that were already in the log before the theory
existed.

The corollary, and it is the one that actually bites: **a new finding is not a
reason to re-order.** Four were found on the night of 17 Sep (G10–G13). None of
them outranks G3 and none of them is allowed to interrupt it.

## The order

1. **G3 / Track A1** — `nv2a_metal_sync_range`. Uninterrupted, first, today.
2. **G3 / Track A2** — extend `owes_guest_ram` to rendered content. Gated on A1.
3. **G10** — the thunk report (~10 min). Then **G11 step 1**, the `XC_AUDIO`
   probe (~5 min). Both are diagnostics, neither changes runtime behaviour.
4. **The mixbin PR to upstream** (~30 min). Owed, ranked first in its own plan,
   still unsent.
5. **G12 and G13's counters** (~10 min each). Both close a question for good if
   they read zero.
6. **G1** — and only by the method in *Rules*, below: a measurement, not a
   mechanism.

G2 and G4 are not in this order. They are parked, with reasons, at their
entries.

---

## G1 — Find out why the guest stops issuing APU methods *(open, method-gated)*

Two minutes into gameplay, every session, `[APU-VOICE] guest_methods` freezes
and never moves again, and the music dies with it. Everything around it stays
alive. Established on 17 Sep, and none of it is in doubt:

- The guest is **alive and still faulting** on the APU aperture. `[MCPX-TRAP]`
  `vp` is frozen at 23,212 and equals `guest_methods` exactly, so nothing is
  lost between trap and model; `apu` keeps climbing ~1,680 per report.
- `[APU-WRITE] main=16997 vp=11762 gp=7 ep=8` — the guest services our traps
  through the main registers and **submits no voice work**.
- The trigger is a **burst of VOICE_ON**, which the player located: *"the sound
  breaks up when you speak to gum"*. The storm ramps 6 → 118 → 358 across four
  windows and then outlives the burst for ever.
- The general invariant: **retired voices stay in the list and the guest never
  takes them out.** `[VOICE-TOP]` counts **eight** guest writes to the 3D list
  head in an entire session, against thousands of raises asking for one.

**Done when:** we know why the guest acknowledges a removal request through the
main APU registers and does not perform the removal.

**Next measurement, already specified and not yet built.** The trap carries ONE
handle in FEDECPARAM. When several voices retire inside one burst, how many
distinct handles were ever actually *delivered* to the guest, against how many
went idle? If the answer is "one per burst", the others were never named and
the guest cannot remove what it was never told about. The `seen`/`delivered`
pair exists and was re-anchored in `5e47836`; it needs a run.

### G1a — `RECOMP_APU_FEDEC_HOLD` default *(DONE, one confirmation owed)*

Defaulted on 17 Sep with the empty-value-safe grammar and a test registered
twice, both arms asserted, verified by injected fault. Fixed a player-confirmed
crash inside the guest's DirectSound ISR (`held=1702` of 1,702, no crash).

**Still owed:** a second player session. One session is a confirmation, not a
measurement, and the crash it fixed is intermittent.

### G1b — Why retired voices stay linked *(open)*

**Done when:** we know what makes a retired, inactive voice stay in the list.

`RECOMP_APU_IDLE_TRAP_SELFLINK` was built for the special case and **refuted by
its own counter the same afternoon** — `selflink_raises_withheld=0 of 0`, the
switch never encountered a self-linked voice, the music still stopped. The code
and test stay because they are correct for the state they describe and cost
nothing off. **Do not re-arm it** without a session showing `encounters`
moving.

### G1c — The music decays, it does not cut out *(open)*

`[APU-BIN] 2D heard` falls +15008 → +14476 → +13610 → +8736 → 0 over ~30 s.
Not the APU falling behind: `[APU-FRAME]` steady ~7,500 frames/window, `se`
91–95%, `trapped` flat 32–36% across the same windows.

**Done when:** we know what declines. A decay points at starvation, and the
per-voice breakdown is what separates "fewer voices contributing" from "one
voice fed less".

### G1d — The storm precedes the collapse *(open, one session)*

The 09:43 session has a pre-storm baseline the 09:07 session lacked: eight
quiet windows, then the storm, then the guest dead within six — and the guest
working **four times harder than baseline** in the two windows before it
collapses. 3D stops dead, then 2D decays out over two windows.

**Done when:** a second session reproduces the ordering. One session is one
session, and the storm and the collapse could still share an upstream cause.

## G2 — The glyph index error *(open, parked)*

`"Let's see how much air $ou can grab"` — one glyph's quad drawn with another
glyph's texture coordinates inside a single batch. Wider than the tutorial:
trick-name overlays show it too.

**Parked, and here is the honest reason:** it is rarer and cosmetic, the
flicker is constant, and the flicker may fall out of G3/A1 for free.

**The label trap is dead and must not be trusted.** Measured 17 Sep from the
player's own dumps: the watched region alternates strictly ABABAB by frame
index across 39 dumps, two distinct images, byte-identical within each parity.
Rendered and looked at: odd frames are the `Gum` label, even frames are solid
black. It fires every frame and catches nothing.

**Re-arm as:** frame N against frame **N−2** (same buffer), or key on the
surface address rather than the frame counter. Until then a `watch*.bmp` dump
is evidence of nothing, and the 168 + 58 frames captured on 17 Sep are not
usable as candidates.

**Done when:** the player sees clean text across a session, and a scripted
pixel diff of a text frame between CPU and GPU arms is empty.

## G3 — The frame tail, and the surface the presenter never gets *(FIRST)*

**This is the work. It is measured, unblocked, needs no player, and has a
correctness bug attached to it.**

`snapshot_surface()` passes the real flipped range to `nv2a_gpu_sync_range`,
and on macOS that macro is `#define nv2a_gpu_sync_range(target, bytes)
nv2a_metal_sync()` — **the range is discarded**. Metal writes back exactly one
surface, the bound one, and the live surface is never the one being flipped.
So the presenter asks for the pixels at address X and receives whatever surface
happened to be bound.

That is one root under two symptoms: a label present on one frame and absent
on the next is exactly what reading an un-written-back surface every other
frame looks like, and the missing range-aware writeback is also what makes
deferring the swap unsafe.

**A1. `nv2a_metal_sync_range(uint8_t *target, size_t bytes)`.** Walk the
surface cache, write back every slot whose guest range overlaps, then point the
macro at it. Mirror `nv2a_d3d11.c:1224 sync_range_inner`, which already does
this and is the proof the shape is right.
*Risk:* low — it writes back MORE than today, never less.
*Test:* `jsrf_metal_copy_test` plus the `[d3d8_gl]` blit check.

**A2. Extend `owes_guest_ram` from clears to rendered content.** Gated on A1;
A1 is what makes it safe. 18,942 surface swaps against 9,504 flip syncs, and
18,937 of 18,942 are rebinds at a 100% cache hit rate.

**Done when:** median frame under 16.68 ms in a mission, verified with the
`[d3d8_gl]` blit check.

**State the caveat before anyone measures it:** `[SYNC]` p99 is 16.0 ms while
`[NOSYNC]` p99 is 29.5 ms. This buys the **median**, not the hitches. Do not
promise that it fixes the stutter.

## G4 — The `RECOMP_SYNC_HIST` halt *(open, shipped OFF, parked)*

One halt in 4 runs with the instrument on, 0 in 4 with it off. The 17 Sep A/B
did **not** reproduce it: three trials per arm, all six reached scene=30, every
run printed its final flip count once. Arms verified distinct by hand.

**Done when:** either an explanation, or an n large enough to exonerate it.
**Until then it stays off.** An unexplained halt seen once is not exonerated by
three clean runs; it is merely not reproduced. Parked because neither outcome
is worth buying yet.

*A harness bug this exposed, fixed:* a switch whose token only prints when ON
gives the off arm nothing to match, so `ab_score.py`'s identical-arms VOID
check was silently skipped — the same failure that scored six switches on
16 Sep. `nv2a_pb_exec.c` now prints `sync_hist on|OFF` regardless.

## G5 — Delete or re-word `[APU-POOL] on_2d` *(open, trivial)*

`on_2d` counts the guest **starting** a 2D voice, and a stream starts one voice
then feeds it, so it flatlines during healthy playback, silence, and mid-flight
death alike. It cannot separate the three, which is the only thing it was added
to do.

**Done when:** it is gone, or its line says what it can and cannot separate.

---

## G10 — The thunk table's report is false *(new, 17 Sep night)*

Every run has always printed:

    Thunk table: 143/378 resolved, 235 unresolved
    WARNING: 235 kernel imports are unresolved - game may crash!

Both numbers are wrong and the arithmetic says exactly how. JSRF imports **120**
ordinals, of which **four** do not resolve: 91 `IoDismountVolumeByName`,
144 `KeSetDisableBoostThread`, 204 `NtProtectVirtualMemory`,
232 `NtUserIoApcDispatcher`.

`kernel_thunks.c:456` walks all 378 slots, but the mapped-XBE branch is gated
on `i < thunk_count`. Past slot 120 that test fails and control falls into the
**fallback reference list** — another title's import order, kept for the no-XBE
case. Slots 120–146 get 27 ordinals JSRF never imported, all of which resolve;
slots 147–377 read past the fallback's 147 initialisers, get 0, and log
`Unresolved kernel ordinal 0`.

    116 real resolved  +  27 fallback resolved  = 143
      4 real unresolved +  231 zero slots       = 235
                                    147 + 231   = 378

**Cost today is entirely diagnostic.** `xbox_kernel_thunk_table` is read by
nothing outside `kernel_thunks.c` (checked across `src/`, `templates/`,
`tools/`) and `xbox_unresolved_thunk` has been called **0** times. The bridge
is the live path and it implements 144, 204 and 232 already; only 91 is absent
from both.

**Done when:** the report counts only slots that came from a real ordinal —
`116/120 resolved, 4 unresolved` — and names the four. Plus a test that a short
thunk table does not borrow the fallback.

## G11 — `XC_AUDIO` tells the title the console is mono *(new, hypothesis)*

`kernel_xbox.c` answers index 0x09 with `0x00010001`. The channel field is
`0 = stereo, 1 = mono, 2 = surround`, so that is **mono with AC3 advertised** —
an encoded path we do not have. The comment beside it claims `0x00000001` is
stereo and is simply wrong. `upstream/main` `8a78867` changed this exact
constant to `0x00000000`, independently, with the same reading. This file has
already been burned once by this value: `kernel.h:1032` records Halo booting to
the dashboard because a parental-control query was answered with it.

**This is flagged as a hypothesis and it is gated.** It is not established that
JSRF reads index 0x09 at all — the handled-index log line is `XBOX_LOG_DEBUG`
and the player's runs are at INFO, so no existing log can answer it.

**Step 1 (do this):** promote the line or add a counter, so one run already
being taken says whether 0x09 is read, and how early.
**Step 2 (gated on step 1 reading nonzero, then on a player listen):** flip to
`0x00000000` with a `tests/` case in upstream's shape.

**Done when:** we know whether the title asks how many speakers it has, and
what we answer. **Refutable in one run:** a session that never queries 0x09
kills it and costs nothing.

**Why it is not ranked above G3 despite touching the live defect.** It is a
one-line change with an appealing story, which is the exact shape of the five
theories that died on 17 Sep. Its measurement is five minutes and runs before
anyone believes it.

## G12 — `MmGetPhysicalAddress` returns a virtual address *(new, count first)*

`kernel_bridge.c:2208` returns the guest VA unchanged. Upstream `127f3fa` folds
the contiguous arena (`addr - XBOX_CONTIG_BASE` inside the window). We have
`XBOX_CONTIG_BASE` and use it in five other places in the same file; only this
bridge ignores it. **JSRF imports ordinal 173**, confirmed against the XBE's
import table rather than assumed.

**Not claimed as a live defect.** Whether the guest ever passes a
contiguous-arena address here is unasked. `xbox_memory_layout.c:3286` already
does the inverse fold for the pushbuffer, so a compensating mask elsewhere is
plausible and would make this harmless.

**Done when:** a counter has bucketed the argument by whether it lands in the
contiguous window. **Count before enforcing** — the same rule G7 is parked
under.

## G13 — `fldcw` is recorded and never read *(new, count first)*

The lifter models `fnstcw`/`fldcw` into `g_fp_control_word`
(`lifter.py:3588–3606`). No FIST helper reads it: `RECOMP_F2I16_ROUND`,
`RECOMP_F2I_ROUND` and `RECOMP_F2I64_ROUND` all call `nearbyint` under the
**host's** rounding mode. Upstream `55aa0ba` reads the guest's RC bits.

**The tree's nearest-even argument is sound and must not be undone.**
`lifter.py:3362` records that `sub_0017C3E8` is MSVC's `_ftol2` — 432 call
sites across 129 functions, *the* float→int conversion for the whole image —
and that it does not reprogram the control word: it converts nearest-even and
corrects toward zero in guest arithmetic we lift normally.

The gap is narrower and still real. The generated image contains exactly **two**
`fldcw` sites, both in `sub_0017F03A`, which is the `_control87` pattern
(`new = (new & mask) | (old & ~mask)`, load, return old). So the title *has* a
rounding-mode setter; whether anything calls it with RC ≠ 00 is unmeasured, and
if something does, every `fist` in that window is wrong by up to one unit.

**Done when:** a counter on `g_fp_control_word` writes with `(cw >> 10) & 3`
nonzero has run. If it never fires, write that down in `recomp_types.h` beside
the nearest-even argument and the question is closed for good. If it fires,
`recomp_fist` is the shape of the answer — at our widths and with our
indefinites, not upstream's.

## Carried forward, unchanged and untouched

- **G6** the ADPCM cause (the guard makes it silent, not correct)
- **G7** the page-table bound that cannot fire; count before enforcing
- **G8** the PCM stereo page straddle (latent: failing voices are mono)
- **G9** 128 switches still hand-roll their grammar; ratcheted, migrate at leisure

## What we owe upstream *(unsent, and it is the smallest thing here)*

`PLAN_2026-09-17_UPSTREAM_STATUS_AND_WHAT_WE_OWE.md` ranks the **mixbin
discard** first: `upstream/main:src/apu/apu_dsp.c:150` reads bins 0 and 1
unconditionally and throws away 2–31, so every 3D voice — every sound effect in
any title — is computed correctly and discarded. Ours is
`RECOMP_APU_MIXDOWN_ALL`, default on, and the player's verdict was "sound fx
working". A handful of lines, generally useful, player-confirmed by ear.

PR #57 is merged, so this is an established path and not a cold approach.
`~/jsrf-build/upstream-contrib` is finished work and is the branch to raise it
from.

**No upstream merge.** 14 conflicts including `lifter.py` and `translator.py`,
and merging those invalidates every archived gen. G11 and G12 are two
cherry-picks if their counters justify them — not 80 commits.

---

## R — Refuted. Do not re-derive these.

| Idea | How it died |
|---|---|
| The flip sync is the cost | Per-caller counters: the flip's sync is already clean, 9,490 of 9,504. The surface swap is the cost. |
| Recovering deferred vblanks is worth ~4% | Guest ISR ran at 101 Hz against 59.94; re-delivering manufactures a tick. |
| ADPCM stride from the segment descriptor | A/B printed the counter in neither arm; the scene has no streaming ADPCM voice. |
| `samples_per_block > 1` breaking the block index | `oversize=0` in every archived run. |
| Overlapping ring reservations from a second thread | 0 draws from another thread. Single-thread assumption now verified. |
| Vertex data changing under the glyph batches | Zero changes across every font-page batch. |
| q ≤ 0 texcoords drawn where the CPU drops them | 0 in 445,498 batches, with a test proving the counter can fire. |
| The music death is dropped idle-trap interrupts (G7's pending flag) | `[IRQ-VEC]` shows v1, v3, v5, v6 all delivering steadily PAST the death. The ISRs are running. |
| `g_vector_in_service` leaked, so the APU vector defers forever | Same evidence. Nothing is stuck in service. |
| Trapping the front end idles the sound engine | `[APU-FRAME] trapped_skipped=0`, and `se = total − halted` exactly. `RECOMP_APU_SE_WHILE_TRAPPED` would not have helped. |
| The v1/v3 idle-trap storm is a two-voice list cycle | `[APU-CYCLE] walks_with_a_cycle=0`, `[APU-WALKCAP] hit=0`. The "cycle" was `v3<-v1` and `v1<-v3` aggregated across a whole run — separate raises at different moments, not simultaneous state. |
| The music "slowing" is the APU falling behind | `[APU-FRAME]` steady at ~7,500 frames/window, se 91–95%, trapped flat 32–36%, across the exact windows where `2D heard` decayed from +15,008 to 0. |
| The self-linked head is the mechanism | `selflink_raises_withheld=0 of 0`. The switch engaged and never met a self-linked voice; the music still stopped. One session's instance of something more general. |
| ~~The idle trap triggers the guest freeze~~ **RETRACTED — see G1d** | Refuted on the 09:07 session, where the storm was ALREADY RUNNING before the window examined, so "the guest coexisted with it" was an artifact of looking too late. The 09:43 session has a clean pre-storm baseline and reverses it. |
| **235 kernel imports are unresolved and the game may crash** | **New, 17 Sep night. 231 of the 235 are empty table slots the loop invented; 4 are real and 3 of those are implemented in the bridge. The unresolved handler has been called 0 times. See G10.** |

---

## Rules for this phase

- **De-risked work outranks interesting work.** See *The rule that orders this
  list*. This is the one that cost a whole day.
- **Take the refuting measurement before building the theory.** Three
  hypotheses died on 17 Sep inside an hour, each killed by a counter already in
  the log before the theory existed. "Measure, don't infer" is also a rule about
  the *order of operations*.
- **No audio fix ships without a counter that can refute it *before* the player
  is asked to test it.** That rule turned the self-link theory into a
  ten-minute refutation instead of an evening.
- A player-facing default needs a picture or a listen, not a residual.
- One run per arm is a lean, not a measurement.
- Scene-match every comparison, and check `[APU-VOICE] on=` before scoring an
  arm: 148–453 is gameplay, 4–12 is the attract loop.
- Scripted runs did not catch the glyph defect in twelve attempts; one human
  session caught it repeatedly. For anything needing two minutes of real play,
  **the player's session is the instrument**.
- `pgrep -x jsrf_first_fault` before touching `src/`.
