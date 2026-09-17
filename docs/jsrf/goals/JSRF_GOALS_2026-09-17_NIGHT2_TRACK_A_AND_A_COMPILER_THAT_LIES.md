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

- **G1a** `RECOMP_APU_FEDEC_HOLD` default — **DONE**, one player confirmation
  still owed; the crash it fixed is intermittent.
- **G1b** why retired voices stay linked — open. `RECOMP_APU_IDLE_TRAP_SELFLINK`
  was **refuted by its own counter** (`0 of 0`) and must not be re-armed
  without a session showing `encounters` moving.
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

**Step 2, unblocked and still gated on a listen:** flip to `0x00000000` with a
`tests/` case, ship it, and ask. Not on the strength of the encoding alone.

**Done when:** the player has listened to a build that answers stereo PCM.

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
| The v1/v3 storm is a two-voice list cycle | `walks_with_a_cycle=0`, `[APU-WALKCAP] hit=0`. The "cycle" was two per-event fields aggregated across a run. |
| The music "slowing" is the APU falling behind | `[APU-FRAME]` steady ~7,500 frames/window across the exact windows where `2D heard` decayed to 0. |
| The self-linked head is the mechanism | `selflink_raises_withheld=0 of 0`. The switch engaged and never met one. |
| ~~The idle trap triggers the guest freeze~~ RETRACTED — see G1d | Refuted on a session where the storm was already running before the window examined. The 09:43 session has a clean baseline and reverses it. |
| 235 kernel imports are unresolved and the game may crash | 231 of 235 are empty slots the loop invented; 4 are real and 3 are implemented in the bridge. The handler has been called 0 times. See G10. |
| **Upstream's DirectSound work could help the music** | **New, 17 Sep. Upstream has exactly one dsound fix ever (`2d2d5e4`, cursor sync) and we already have it — it predates our merge base. And it could not matter: `src/audio/dsound_device.c` is NOT LINKED into `jsrf_first_fault` (checked with `nm`; no `DirectSound*` symbol present). JSRF's DirectSound is the title's own XDK code. The other ecosystem audio branches are Media Foundation and XAudio2; our backend is SDL2.** |
| **Upstream's contiguous-memory heap bug (PR #60) is live here** | **New, 17 Sep. Real upstream — POSIX `VirtualQuery` hardcodes `AllocationBase=NULL`, so `MmFreeContiguousMemory` always `free()`s an mmap'd pointer — and we have the identical code. But our bridge routes ordinals 165/171 to `xbox_ContiguousAlloc`/`xbox_HeapFree` (guest arena, canonicalised, instrumented). The broken pair is referenced only by the dead thunk table. Latent, not live.** |
| **The discarded flip range is what makes the label flicker** | **New, 17 Sep. Retracted by A1's own commit (`2b5bdb2`) after the afternoon plan asserted it: every draw sets `surface_dirty` and every swap syncs before it rebinds, so guest RAM is already current for every surface and the range walk finds nothing to pay. A1 is infrastructure for A2, not a fix the player can see.** |
| **Track A is unstarted and A1 must be built** | **New, 17 Sep night. Said by the afternoon plan AND by the 19:48 goals file, and false in both: A1 and A2 landed at 18:35 as `2b5bdb2` and `ff07677`, with three registered tests. Two documents agreeing does not outrank the commit log.** |
| **G14's SF bug is live in the player's build** | **New, 17 Sep night, and it was MY claim two hours earlier. The UB form appears at 5 sites and all 5 read `cmp <mem>, 0`, where the subtraction cannot overflow. The other 696 SF consumers use `TEST_S` or a sign-of-one-value form, neither of which subtracts. I counted one expression form, called it the site count, and never read the operands. The defect is real; its reachability here is zero.** |
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
