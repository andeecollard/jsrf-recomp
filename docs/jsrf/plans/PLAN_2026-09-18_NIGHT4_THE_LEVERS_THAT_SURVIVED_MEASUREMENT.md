# The levers that survived measurement — 18 September 2026, night (4)

Supersedes `PLAN_2026-09-18_NIGHT3_TWO_GOALS_AND_ONE_REGENERATION.md`. That
plan's two goals stand and its cost asymmetry stands. Its **ordering does not**:
its top graphics item is refuted and its top recompiler item is an order of
magnitude smaller than it claims. Both were measured tonight rather than argued.

The goals are unchanged and still the player's words:

> **A. Get JSRF running properly** — everything on the GPU, 60 fps, correct speed.
> **B. Improve xboxrecomp** — a better recompiler, upstreamable, not JSRF-shaped.

## What tonight measured, and what it cost the last plan

| claim in NIGHT3 | what the measurement says |
|---|---|
| NV2A descriptor table is "the biggest lever on both frame time and the glyphs" | **~0.4 ms of a 32.8 ms frame.** Dead as a frame-time lever. |
| "a ~900-line `switch`" | 895 lines, **23 case labels**, fronted by 7 range tests and tailed by 3. A comparison chain, ~6 ns per method. |
| dead-flag elision is "the highest-value pure-performance item" | **12–16% of flag sites but 1.0–1.5% of guest loads.** ~88% is stores clang already deletes at `-O2`. |
| `irq_latency` is "written but not wired" | **Stale.** `7c8ae51` wired it: raise at `qemu_shim.h:202`, deliver at `kernel_bridge.c:2949`, report at `:7843`. |
| IEN: "ours gates on it, Microsoft's does not" | True, but **"ours" is xemu's, character-identical**, and `apu_regs.h` is 366/366 lines identical. Prior lowered, not refuted. |
| §10.2 "ours raises once" | **False.** Implemented twice — the default walk re-raises at 1,500 Hz, and the player's live `IDLE_TRAP_EDGE` + 16 ms rearm *is* Microsoft's design. |
| §10.7 recognise DSP effects instead of emulating them | **Cannot help this title.** `[APU-WRITE] gp=7 ep=8` in two complete sessions — 15 writes at init, never again. |

### The one genuinely new finding, and its refutation in the same night

**Every frame makes a GPU → CPU → GPU round trip.** Metal renders; at
`NV097_FLIP_STALL` `snapshot_surface()` waits for the GPU and reads the whole
colour surface into guest RAM; `d3d8_gl.c` converts it and uploads it back with
`glTexSubImage2D`. `nv2a_metal.m` has no `presentDrawable`, no `CAMetalLayer`.
That readback is `[STAGE] sync`, one call per frame, and it is **5.19 ms of a
16.43 ms player frame — 32%**, the largest identified stage.

**And it is not recoverable by removing the trip.** `RECOMP_METAL_NO_FLIP_SYNC`,
3 trials per arm, ABBA, binary pinned, arms verified distinct:

    =0  27.04 28.46 30.76 ms      =1  27.48 27.97 32.00 ms    ranges overlap
    =0  submit= 4.52 sync= 9.92   =1  submit=15.01 sync=0.01
    =0  submit= 5.58 sync=10.68   =1  submit=15.27 sync=0.01
    =0  submit= 4.85 sync=10.81   =1  submit=15.79 sync=0.01

`sync` goes to zero and `submit` absorbs it, near enough one for one. The wait
is **the GPU finishing the frame**, not an artificial serialisation. Direct
Metal presentation still removes the 4.9 MB copy, the conversion and the
re-upload — but **size it from those, never from `[STAGE] sync`**.

---

## THE ORDER

### 0. The session. Still gated on the player, still the only thing that answers G1.

Five instruments armed since 19:58 and **none has been read**; `last-run.log` has
not moved since 18:17. Nothing below advances G1. Reach gameplay (`on=` in the
148–453 band) and play past t=360 s.

**There is now a specific hypothesis for it to test**, and it is the first
guest-side gate anyone has found with the right shape. `sub_001A6064`
(`gen/recomp_0008.c:49873`) **early-returns whenever `FECTL & 0xE0 != 0`** — a
guest routine that refuses to submit while the front end is HALTED *or* TRAPPED
— and then spins on `NV1BA0_PIO_FREE`. Set that beside `MS_KERNEL_AND_XAM.md`
§10.1: *a guest thread spinning in a loop with no kernel calls is uninterruptible
for us*, because our interrupts only run when a thread calls a wait function.

That is a complete mechanism: guest waits for the front end to go free, spins
without calling the kernel, we never deliver the interrupt that would free it.
`RECOMP_APU_TRAP_THREADS` beside `RECOMP_KERNEL_THREADS` discriminates it — the
submitting thread's `vp=` stops while `[KERNEL-THREADS]` shows it still calling
is "stopped asking"; both stopping is "stopped being scheduled"; and a thread
whose kernel-call rate *drops but does not stop* is the spin.

Note the 17:55 session already shows one thread (`tib=0x00982000`) dropping ~35%
at exactly t=270 and holding. That is the candidate.

**Done when:** G1 is one of those three and not the others.

### 1. The colour resolve A/B *(running)*

`RECOMP_METAL_NO_COLOUR_SYNC`, the same question `no_depth_sync` won 9.1% with,
sized from a control run at **16,883 colour write-backs** against depth's 16,872.
The only graphics lever left with direct evidence behind it.

**Caveat carried from item 0's refutation:** if this also moves the cost to
`submit` rather than removing it, read the stage line before the frame line.
The depth win was real, so the mechanism differs — a resolve that is never
performed is not the same as a wait that is deferred — but check, do not assume.

### 2. The cursor pin — the best audio arm we have ever had *(§10.5)*

Promoted from "cheapest arm in the document" to **proven from the guest binary**.
`sub_001A2E2E` — the crash function, named by 40 of 66 guest faults — reads
`CVL`/`NVL` at every removal and branches on them (`gen/recomp_0008.c:40858`).
Handles are 0…255, so with both pinned at `0xFFFF` the compare can never match
and the guest takes a two-instruction skip; **the unlink itself happens earlier
and unconditionally**. Our model never reads those registers — the walk uses a
local cursor — so the pin changes exactly one thing: what the guest sees.

83 of 87 removals took the repair branch against us. Microsoft's titles take it
on **zero**.

- Switch `RECOMP_APU_CURSOR_PIN`, `recomp_switch_on`, default OFF.
- **Both halves are required**: stop republishing the five writes *and* drop
  guest writes to those six offsets in `mcpx_apu_write`. Without the second, a
  guest write that lands sticks for ever and the pin silently un-pins itself.
- Leave `TVL2D/3D/MP` alone in both directions.
- **Proof it took, needing no new code:** `[VOICE-TOP-RING] on-trapped-voice`
  must go 83 → 0.
- **Safety counters that must not move:** `g_apu_top_write_count[3]`,
  `g_top_unlink`. If head writes or textbook unlinks fall, the pin broke removal.
- **Refutation:** OFF-arm `writes=0` or `reads=0` means the driver never took
  the branch this session and the pin removes something nobody executes.

### 3. Arm `recomp_gpu_own` on Metal — the instrument we are missing *(no behaviour change)*

`src/kernel/recomp_gpu_own.h` detects a guest read of GPU-resident memory. It is
armed on the D3D11 path (`nv2a_d3d11.c:1192, 1205, 1217`) and **never on Metal**
— zero calls in `nv2a_metal.m`. So on this host the map is all-zero, the seam is
a not-taken branch, and there is no report line at all.

Consequence: **we cannot say whether the guest reads the framebuffer**, and we
are safe today only because Metal writes guest RAM back eagerly. Every deferral
idea above and below depends on that answer.

`[GPU-OWN] touches > 0` is the positive control; `hits = 0` with `touches > 0`
is then a real absence measurement. `touches = 0` means the instrument is dead.

### 4. Dead-flag elision — prove it WITHOUT spending a regeneration

The gen tree is not in git and not bit-stable, but it **is a text corpus**, and
the analysis is a pure filter over it. Copy it, delete the dead pure-flag
statements — the same transformation the lifter change would make — build both,
A/B. If it does not move frame time, the item dies for the cost of one build and
the regeneration slot goes to the poll byte and the flat table.

If it does move: 62% of the paying opportunity is **`inc`/`dec` with a memory
operand**, fixable in `_lift_inc_dec` alone with a stronger safety argument than
the general pass has.

For scale, from this tree's own notes: `_tlv_get_addr` is **~75% of the main
guest thread's samples**. Dead flags are ~1% of guest loads. Do not confuse them.

### 5. THE ONE REGENERATION — gated on 4, and on reading `[IRQ-LATENCY]`

Unchanged from NIGHT3 except that dead-flag elision is now conditional:

1. **The poll byte**, gated on `[IRQ-LATENCY]`. Microseconds → it buys nothing
   and the regeneration is saved. Milliseconds → it is demonstrated.
   `MS_KERNEL_AND_XAM.md` §10.3 adds a correctness argument NIGHT3 did not have:
   they emit the poll immediately **before** each interrupt-disable escape, so a
   pending interrupt is never deferred across a `cli`.
2. **Separate "advance time" from "deliver interrupts."** §10.5 says this is
   *probably worth more than shortening the period*. Our 8 ms tick does both.
3. **`g_flat_table` at basic-block granularity** — structure exists, the work is
   entries. **But see the conflict below before ranking it.**
4. **`EFLAGS` only at `pushfd`/`popfd`.** If this lands with item 4 above,
   liveness must treat `pushfd` as a use of every flag from the first commit.
5. Dead-flag elision, only if 4 says it moves.

**Re-verify the baseline immediately after.** Every archived measurement
predates it.

### 6. Upstream

- **Sent tonight:** [#70](https://github.com/sp00nznet/xboxrecomp/pull/70)
  (`bts`/`btr`/`btc` CF) and
  [#71](https://github.com/sp00nznet/xboxrecomp/pull/71) (SF signed-overflow UB).
  With #67 and #69 that is four open. Maintainer quiet since 16 Sep.
- **`popfd` is in `_EFLAGS_PRESERVE`** (`lifter.py:349`) so flag state survives
  an instruction that overwrites every flag. The `neg` peephole already excludes
  it (`lifter.py:3750`); the tracking loop does not. One line.
- **The result-setter clobber class.** `and`/`or`/`xor`/`add`/`sub`/`neg`/shifts
  still rebuild conditions by re-reading a possibly-modified destination
  (`lifter.py:436-438`). This is the bug class `_snapshot_flags` closed for
  `cmp`/`test`, and that `inc`/`dec` were explicitly moved off after it hung
  JSRF's ADX loop indefinitely. **A real bug hunt, not a one-liner**, and the
  highest-value thing we owe upstream.
- `get_data_ptr`'s dead bound, as an issue.

---

## The conflict between two BC documents, and which one wins

`ms-fusion-codegen-teardown.md` Tier 1 calls basic-block translation units "the
one architectural change worth making". `ms-fusion-adoption-plan.md` rejects
block granularity **on measurement**: for Halo, 0 of 46 unresolved stubs and 1 of
25 indirect targets were mid-function. It would have fixed about one address, at
a C call per 10–20 guest instructions.

They are not quite the same claim — one is about entry points, one about
translation units — but the measured one wins on ranking. Item 5.3 is worth doing
as *density*, not as an architecture change.

---

## Refuted tonight. Do not re-derive.

| idea | how it died |
|---|---|
| The NV2A descriptor table is the frame-time lever | `walk` minus its nested stages minus instrumentation is ~0.4 ms of a 32.8 ms frame, over 64,617 methods/frame. Below the resolution of the instrument measuring it. |
| Removing the flip read-back recovers 5.19 ms | A/B, arms verified distinct: `sync` → 0.01 and `submit` absorbs it one for one. The wait is the GPU, not the copy. |
| Recognising DSP effect programs could help the music | `gp=7 ep=8` in two complete sessions. JSRF never uses the GP/EP DSPs. |
| The FECTL HALTED divergence is the freeze | Real — 124,443 frames where xemu raises `FETINTSTS` and we do not — but `halted` drifts smoothly ~275→570 per window across the whole run with **no step at t=270**, while `guest_methods` goes 26 → 0 and stays. |
| Our IEN gate is a local invention | `apu_regs.h` 366/366 identical to xemu's; `update_irq` character-identical. It is xemu's gate. Caveat: nobody has checked whether xemu keeps JSRF's music alive past t=270, so this lowers the prior rather than closing it. |
| "Ours raises the idle trap once" | Twice over. The default walk re-raises at 1,500 Hz; the player's live config is Microsoft's own per-voice tick-stamp design. |
| XAM has anything for us | Fully analysed, 11.3 MB. Not input, not audio, not saves, not pacing. Closed. |

## Rules this night added

- **A stage's size is not a lever's size.** `[STAGE] sync` was 32% of the frame
  and worth nothing to remove. Measure the *delta*, not the *total*.
- **Measure with the operands the program actually has.** Checking the SF defect
  with literal arguments got it constant-folded and reported "no problem at any
  `-O`". Guest operands are never literals.
- **A registered token is not a harvested token.** `ab_score.py` read the switch
  state only from `[METAL]` lines; three A/Bs have now been scored with the
  VOID check silently skipped. Fixed to scan every line.
- **Read the guest binary before theorising about the guest.** It is on this
  machine, recompiled and readable, and it settled §10.5 and refuted §10.2 in an
  afternoon after weeks of inference.
