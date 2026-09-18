# G1 is refuted and the guest stops allocating — 18 September 2026, night (2)

Written after a day that refuted the invariant the music work was built on,
found a new defect that stops play outright, discovered the headline timing
experiment cannot run in the shipping build at all, and closed two research
lines for good.

Supersedes `PLAN_2026-09-18_NIGHT_THE_CRASH_IS_THE_BOTTLENECK.md`. That plan's
Track A is answered (the crash's `this` question) and its premise has moved: the
crash did not fire once in either of today's player sessions.

## The scoreboard

| defect | player sees | status tonight |
|---|---|---|
| **freeze** | screen goes black ~6 min in, process alive | **NEW. Instrument built and armed; one session read pending** |
| music | dies ~6 min in, every session | **G1's invariant is REFUTED. Needs re-founding, not extending** |
| text | wrong glyphs in speech boxes | open; now scene-located at Gum, BMPs unspent |
| speed | 19 ms against a 16.68 ms budget | 9.1% shipped; the drain is a stall, D1 never run |
| crashes | occasional `sub_001A2E2E` | **did not fire in either player session today** |

## The rule this plan is trying to obey

**A refuted entry left standing costs a whole session.** The previous handover
shipped beside a plan its own commits had already refuted, and said so. G1 is
now in that state. Rewriting it is cheap, it needs no run, and it protects every
session after — so it goes first, ahead of work that is more interesting.

---

## 0. READ, 17:55 — answered, and it corrected the plan's own premise

The session ran **1,523 s and never went black**. The kernel census answered
what it was built to answer and the answer is negative in a useful way:

- **No thread dies.** Four threads alive with near-constant rates 1,250 s after
  the APU froze; a fifth dormant since t≈0.
- **The APU freeze reproduces** at t=270 s, and rendering continued untouched.
- **The eleven-ordinal census is NOT the freeze.** All eleven stop here too,
  across 6.7 M calls, in a healthy run. That was steady state. The 14:59 reading
  of it was wrong for want of a control, which is corrected in the progress note.
- **Scene caveat:** `on=27` here against `on=249` at 14:59, so this does not
  show the black screen is non-deterministic — only that it does not follow from
  the APU freeze.
- **One signal:** `tib=0x00982000` drops ~35% at t=270 and holds.

**Why it stops there:** APU submission is MMIO into the trapped aperture, not a
kernel call, so no ordinal histogram can see it. The next instrument is
per-thread attribution on the **MCPX trap** — same shape, same `g_fs_base`,
different handler. That is now item 0b.

## 0b. Per-thread MCPX aperture attribution *(next build)*

**Done when:** a run says which guest thread submits APU methods, and whether at
the freeze it stops submitting or stops being scheduled.

## ~~0. Read the session in flight~~ *(done, see above)*

`RECOMP_KERNEL_THREADS` is armed and confirmed live (5 distinct guest threads,
0 overflowing a 16-slot table). When the run ends, `[KERNEL-THREADS]` names the
thread that went quiet and its last kernel call.

**Both outcomes are findings.** The 14:59 session went black at t=360 s and its
log reached 6.07 MB; this one passed 8.0 MB still running. If it does not freeze,
the freeze is not deterministic, and a non-deterministic freeze is chased
differently from a reproducible one.

The same read gets a second VOICE-TOP ring sample and FB-WATCH counts across the
Gum text the player hit.

## 1. Re-found G1 *(no run, no build — do this first)*

The goals entry asserts:

> retired voices stay in the list and the guest never takes them out — eight
> guest writes to the 3D list head in a whole session against thousands of
> raises

Measured today, and in the 11:47 reference session:

    [VOICE-TOP-RING] 87 head writes: unlink=81 during-trap=83 on-trapped-voice=83
                     still-active=7 self-linked=0 into-empty=3 emptied-list=7
    [APU-IDLE-DELIVERY] found idle=19 distinct, delivered=19 distinct, never told about=0
    reference 11:47:   found idle=15 distinct, delivered=15 distinct, never told about=0
    [VOICE-TOP] 2D=3 3D=83 MP=1   (reference: 2D=5 3D=26 MP=1)

**81 of 87 head writes are textbook unlinks. 83 of 87 happened while the front
end was trapped with `cvl` naming exactly the removed voice** — the guest
removed precisely the voice the idle trap told it about. `self-linked=0`. Every
idle handle was delivered, in both sessions.

Both halves of "the guest acknowledges a removal request and does not perform
the removal" are false.

**Caveat to carry into the rewrite:** today's arms include `CYCLE_BREAK`,
`FEDEC_HOLD`, `SELFLINK_END` and `LIST_MOVE_TO_FRONT`. This is that stack's
behaviour, not bare behaviour. The honest new question is *what the music
actually does between t=0 and the freeze*, not why removals fail.

**Done when:** G1 in the goals file describes what is measured, and the seven
dead hypotheses are in *R* rather than in the entry.

## 2. `-DXBOX_WORKER_STACK_COUNT=1` — BUILT 18 Sep night *(ready, needs a session)*

    binary: /Users/andrewcollard/jsrf-build/jsrf-first-fault/build-irqthread/
    bundle: /Users/andrewcollard/jsrf-build/irqthread/JSRF.app      <-- NOT the main one

Kept in its own directory so the wrong bundle cannot be launched by accident.
60/60 ctest in that build; 60/60 also in the default build, unchanged.

**A test now proves which binary you are holding.** `jsrf_worker_stack_pool`
asserts the allocator against the count the build actually compiled in, so it
passes in BOTH configurations and says something different in each:

    build-feav       XBOX_WORKER_STACK_COUNT=0   pool absent: alloc refuses
                     "RECOMP_IRQ_THREAD cannot deliver in this build."
    build-irqthread  XBOX_WORKER_STACK_COUNT=1   every slice allocated, reuse ok
                     "RECOMP_IRQ_THREAD can deliver in this build."

That exists because on 18 Sep the switch was armed for a whole player session and
delivered nothing, and the failure looked exactly like a switch that found
nothing to report. The pool had no test at all.

### (original note retained)

    [IRQ-THREAD] no worker stack slice (XBOX_WORKER_STACK_COUNT=0); not delivering

`bridge_irq_thread` borrows a guest stack slice; the pool is 0 by default and
`xbox_memory_layout.h:396` records why — 16 slices is 4 MB under the arena and
this title already fails few-hundred-byte allocations with ~2 MB to spare. One
slice is 256 KB and is all the thread takes.

**It must not ride along with a freeze measurement.** It changes the memory
layout and delivery timing globally, and the handover's own caution stands: if
other instruments report something odd, suspect it before believing them.

**Done when:** the `[IRQ-THREAD] live` banner appears and a crash rate is
measured against a shorter raise/dispatch window. If the rate falls, the window
is the mechanism and the fix is timing — tune `BRIDGE_DEVICE_IRQ_PERIOD_MS`, do
not add a guard. If it does not move, the dispatch-time check at
`bridge_deliver_isr` earns its turn.

## 3. The two live defects

### G2 — glyphs, now scene-located

The player hit it at Gum this session, so it has a reproducible location for the
first time. FB-WATCH has read real counts (31,643 comparisons, 17,872 changed,
391 small) with `comparisons>0` as its positive control.

**DO NOT simply uncomment `RECOMP_FB_WATCH_DUMP`.** Measured 18 Sep night from
the 17:55 log, before spending anything:

- small-change events fire **~100 per minute, uniformly, for the whole run** —
  2,000 of them between t=0 and t=1090. They are ordinary animation, not the
  defect. "Small" is not a proxy for the glyph bug.
- each dump is a **full 640x480 frame, 921,654 bytes** — `dump_snapshot_bmp`
  writes `s_snap_w x s_snap_h` (`s_gpu.clip_w/h`), NOT the 80x35 watch region.
  Verified against existing `watch*.bmp` in `glyphdump-2026-09-17_0907/`.

So any workable cap is exhausted within ~30 s of `RECOMP_FB_WATCH_AFTER`, on
animation, which is exactly the failure `nv2a_pb_exec.c:1830` already records
("59 dumps, all of them legitimate animation, before the window where the defect
was seen"). Arming it blind repeats that.

**Two honest ways forward, pick one:**

1. **Targeted `AFTER`.** The player notes roughly the wall-clock second when the
   Gum text corrupts (the log prints `t=` continuously), then a follow-up run
   sets `RECOMP_FB_WATCH_AFTER` to just before it with a cap of ~30. Costs one
   extra session; needs no code.
2. **Make the trigger selective.** The defect is a wrong glyph in text that
   should be STATIC between frames, so the discriminator is "this region changed
   while the scene was otherwise still", not "the change was small". That is a
   code change to the trap, and it is the one that would make a scripted run able
   to catch what twelve of them have missed.

Twelve scripted runs never caught this and one human session catches it
repeatedly — which is also the argument for phobos665's D3D8 capture/replay
tool under *what is worth taking from them*.

### G1 — the music, against xemu's `vp.c`

Our APU is xemu's, extracted. Seven hypotheses have died and none was checked
against the implementation ours came from. Unread and directly relevant:
voice-list splicing on VOICE_ON (`LIST_MOVE_TO_FRONT` and `SELFLINK_END` are
local inventions with xemu's originals right there), `voice_lock` semantics, and
mixbin handling for 3D voices.

**There is no xemu source on this disk** (`~/Documents/xemu` is empty). Fetching
it is the first step, and it is the next source to read — not PowerPC.

## 4. Speed — the drain is a stall, and D1 is still free

19 ms against 16.68 ms. `NO_DEPTH_SYNC` is shipped and player-confirmed (~9.1%,
eight usable runs, every `=1` below every `=0`). `defer_swap` is refuted and
actively harmful — it moves the cost to the flip.

What is open is the **drain**: 79% of sync time, and from the 11:47 reference
session drain-per-call went 2.74 → 5.17 ms (+89%) while draws fell 18%,
triangles 25% and surface rebuilds stayed at 0. Twenty-five percent less work,
twice the wait. That is a stall, not load.

### D1 — ANSWERED 18 Sep night, free, from the 17:55 session

That session sat in one scene at a flat ~28,500 draws/window for 1,250 s, which
is a better thermal test than the revisit D1 originally proposed — constant
workload, twenty minutes:

    t= 300- 450   frame 16.84 ms   28,300 draws/win   drain/call 1.646 ms
    t= 750- 900         16.46      28,870                        1.618
    t=1200-1350         16.42      28,941                        1.635
    t=1350-1500         16.64      28,571                        1.649

    DELTA over 15 min:  frame -0.29 ms (-1.7%)   drain/call -0.003 ms (-0.2%)

**Thermal is ruled out.** Twenty minutes of sustained load produced no
degradation whatsoever — it drifted very slightly faster. So the 11:47 session's
drain-per-call going 2.74 → 5.17 ms (+89%) on 25% LESS work is **scene-driven**,
and the stall has a cause inside the scene rather than in the machine.

**And this session held 16.4–16.8 ms — at the 16.68 ms budget.** We reach 60 fps
in a light scene. The 19–21 ms figure is scene-dependent and is not a global
deficit, which reframes the goal: find what the heavy scene does to the drain,
rather than hunt for a uniform 15% saving.

Caveat: `on=27` here against `on=249` in a mission, so this is a light scene by
the audio measure. It rules thermal out; it does not characterise the heavy one.

## 5. Owed upstream *(cheap, not urgent)*

1. **G15** — `bts`/`btr`/`btc` report CF after their own write. Fuzzer-found,
   latent for us.
2. **G16** — narrow rotates; completes PR #57's story.
3. **`get_data_ptr`'s dead bound** (our G7), as an issue rather than a patch.

PR #67 and #69 are open, zero comments, maintainer quiet since 16 Sep.
`~/jsrf-build/upstream-contrib` is the place to raise these from.

---

## Closed today — do not reopen without new input

**The Hacked Xefu Pack.** Previously closed on "JSRF's title ID is absent from
its 116 configs". That was the wrong ID: the BC programme supported the bundle
disc, whose launcher is `4D53003D` (confirmed by parsing it) while JSRF's own
executable is `5345000A`, the same as ours. Writing an LZX decoder got the
primary source out of Microsoft's own `xefutitle*.xex` data DLLs: JSRF enters
the BC title table at the XDK 5426 generation and carries an **all-`0xFF`
payload in every version**, where 16 of ~130 records do carry data and every
audio-fixing config in the corpus sets fields JSRF's does not. Microsoft shipped
this title on the defaults. There is nothing to borrow.

**ms-fusion via the bundle disc.** `SegaGT.xbe` links XDK **4627**, not 4134, so
it is not a signature donor. `SegaJSRF.xbe` IS a second build of our game
(2002-01-15 against our 2002-01-28, same XDK 4134) whose library sections differ
from ours by **relocation only** — DSOUND's 636 differing runs are all a +0x40
dword delta bar one `E8` rel32. That is a free offline oracle on our
disassembly of the section the crash lives in, recorded at
`~/jsrf/xex_tools/README.md` and in memory. `.text` differs 69.7%, so it is not
a cheap A/B.

**`xefu7.xex` is fully decompressed** and verified three ways — our decoder,
upstream libmspack unmodified, and an independent clean-room implementation, all
byte-identical across 6,291,456 bytes. Contents are thin: a release build, no
symbols, no emulation-internals strings, host-side D3D9 text throughout. The one
Xbox-1 find is the APU Global Processor DSP effects table (I3DL2 reverb with ~20
presets, Voice Rate Converter, oscillators, mixers, flange, echo, chorus,
ampmod, distortion, delay), which confirms the effect set we have **stubbed**
but does not touch voice-list splicing. Anything further is reverse-engineering
1.19 MB of unsymbolised PowerPC with 2433 function boundaries and no strings to
anchor them.

## The order, if you want one line

Read the session → rewrite G1 → build the worker slice and run the IRQ-thread
arm → spend G2's BMPs at Gum → fetch xemu and read `vp.c` → D1 on any session →
upstream debt.
