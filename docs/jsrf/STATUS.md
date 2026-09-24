# Where Jet Set Radio Future has got to

Last measured 14 September 2026, against the tree at `a113ae9`, title built
`-O2`, on an Apple M1 Max, with the 21 September section below added against
`49ff7e2`, and the drain, vertex-path, batching-default and ADX entries
corrected on the night of 21 September against the 1167 s player session in
`progress/CLAUDE_PROGRESS_2026-09-21_NIGHT5_THE_STEAL_PUT_THE_POISON_BACK.md`.
Every number here came from a run; where something is believed rather than
measured it says so.

## Summary, 25 September 2026

Newest first; the sections below it are the older measured record and remain
true unless this summary says otherwise. The plan and the evidence behind
each line are in `goals/JSRF_GOALS_2026-09-24_NIGHT_EVERY_SCENE_IS_REACHABLE.md`.
**Measured** means a run or a player session produced the number;
**observed** means seen on screen without a controlled comparison.

| area | state | basis |
|---|---|---|
| story progression | playable into chapter 2; the save decodes to chapter 2 mission 240, 6 characters, 23 souls, Rokkaku-dai 16 Poison Jam tags still to cover | measured: save decoder, byte-for-byte round trip |
| every chapter reachable | chapters 2 (missions 0, 10, 40, 96) and 5 (10) entered unattended with no fault | measured: `RECOMP_CHAPTER_JUMP` runs |
| cutscene one-frame dropouts | fixed (G57, `frndint` rounding control); chapter-2 intro 11 transient frames → 0 | measured: flight recorder, before/after on one build |
| Rokkaku-dai city | fixed (G54) | player session |
| characters whited out in Rokkaku-dai | fixed (G58, 16-bit swizzled textures on the GPU) | player session |
| stage music dying after ~2 min | fixed (DSOUND lift, 23 Sep); stage track read in real time to the end of a 1,370 s session; 14 small underruns in it | measured: ADX reads matched against the file; content not checked by ear |
| graffiti studio cannot paint | fixed in code (G59, linear 32-bit textures), not yet confirmed | measured: the dropped draws were format 0x12 in the player's log |
| Rokkaku-dai water looks like a void | bump mapping implemented (G60), not yet confirmed; whether it then looks right needs an xemu reference | measured: the water is rasterised where the void is (`RECOMP_MARK_BUMP`) |
| cutscene sweep | in progress over the 74 cutscenes reachable by a jump | — |
| corrupt glyphs in speech boxes / trick names (G2) | open, not re-measured since 21 Sep | — |
| Poison Jam chase cutscene elements | open; needs the chase played to reach | — |
| boost flicker | open, not re-tested since G57 | — |
| frame time | tutorial ≈ 11.5 ms (86 fps uncapped) with combiner specialisation; heavy scenes last measured before it at a 50 fps median | measured 24 Sep / 23 Sep; heavy scenes need re-measuring |
| fault rate | not re-measured since the 21 Sep figures below | — |

## Fixed 21 September 2026 — the Load screen, the light, and the character select

Three menu screens that were black or wrong now match xemu, each confirmed
on screen by the player in the same session (`49ff7e2`, build
`5E867133`). The account is
`handovers/HANDOVER_2026-09-21_NIGHT4_THREE_FIXES_AND_THE_FORCE_MODES_ONLY_EVER_SHOWED_THE_LAST_DRAW.txt`.

| | what it was | evidence |
|---|---|---|
| Load screen black | every fragment failed the depth test. The per-frame composite binds a back buffer with the render target's depth pointer, so the surface cache held two depth textures for one guest depth buffer and the frame's clear only ever reached one | 24 consecutive draws captured, every triangle accepted, surface unchanged; `RECOMP_METAL_HW_DEPTH_ALWAYS=1` restored the screen in one run; the fix reports 12,896 sibling-slot depth clears in the confirming run |
| Load screen brown | the fixed-function infinite-light dot product was negated; lit surfaces got scene ambient only | xemu's emitter forms `max(0, dot(tNormal, lightDirection))`; the guest's scene ambient on that screen is (0.26,0.13,0), the brown we drew |
| Character select's two 3D panels black | the texture-copy gate refused any draw whose window clip was smaller than the surface | 495,110 "partial window clip" refusals in one 90 s session; 0 after the window clip became a scissor |

Not measured yet: gameplay under the corrected light sign, and whether the
intro's black stretch and the missing fence and graffiti are the same stale
depth copy. The C suite reads 110 of 112 on this host: `jsrf_input_hotplug`
fails whenever a controller is attached during a session, and
`jsrf_switch_audit` carries six hand-rolled switch reads over its ratchet
from before this session.

## Working

| | evidence |
|---|---|
| Boots to gameplay unattended | `pad/gameplay_nobarrage.pad` reached a running mission — `[JSRF-SEQ] now=30`, held 111 s — unattended, 15 Sep 2026. The older "12 of 12 scripted boots reached the title gate (`NtOpenFile` 1342)" is withdrawn: that count was timing the disc cache build and reads 131 in every scene on a pre-cached HDD |
| Renders | 245,331 native draw batches in a 75 s intro run, 0 software fallbacks |
| Audio | output holds 47,602–48,006 Hz across every scene measured, 14 runs |
| Controller input | 7 of 7 full 300 s runs retire USB transfers continuously; the guest's own driver acknowledges ~31,000 done queues per run |
| Tutorial | completes; the title reaches the playable part |
| Tests | 29/29 C tests (the suite gained `jsrf_vsh_msl`), 56/56 translator Python tests — C suite re-run 15 Sep 2026 at `8b11fcc` |

The C suite is green because the checks it used to fail on were RETIRED, not
because they started passing by luck. `CMakeLists.txt` records each one: four
site-specific gates retired on 13 Sep 2026 and two more on 14 Sep, each the
moment the lifter began emitting the fixed form by itself — at which point the
check asserts a pre-fix text that no longer exists and fails FOR the fix. The
unresolved-flags ratchet was re-based the same week: it was set at 76 against a
tree at 90, so it could never pass, and it gated on a total dominated by data
the linear sweep walked into as code. It gates the reachable count now, at 2.

Read the 28 narrowly. They are `diagnostics/jsrf_first_fault`'s, and that is the
whole of the C-side coverage this fork runs: `tests/` at the repository root is
built by nothing — no `add_subdirectory(tests)` exists anywhere, and two of its
four directories have no `CMakeLists.txt` of their own. Green here says nothing
about them. Recorded at `462b656`; the detail is in
`docs/jsrf/EXPERIMENT_CONTROLS.md`.

## Not working

**Frame rate.** 27–30 fps at a scene-verified mission against a title that
holds 60.1 fps in xemu. **That number has not been re-measured since the GPU
vertex path became the default** and should not be quoted as current; the
21 Sep night player session read 43–53 fps in ordinary play and 28–34 with
heavy traffic, but in different scenes, and scene-matching every comparison is
the rule this tree learned the hard way. `measure.sh` on the same scene
settles it.

**THE GPU WORK ITSELF IS THE COST, AND IT IS SOFTWARE TEXTURE SAMPLING.**
Found 21 Sep 2026 night by reading how the Metal backend drives the hardware:

    grep -c 'MTLSamplerState|newSamplerStateWithDescriptor|texture2d<'  ->  0

**Not one hardware sampler exists in the renderer.** `MTLTextureDescriptor`
appears only for the depth texture, the stencil texture and the render
surface — never for a guest texture. Guest textures live in
`const device uchar*` buffers (`[METAL] texture buffers: 13,100,361
requests`) and `sample_lod()` samples them **in software inside the fragment
shader**: perspective divide, `dfdx`/`dfdy` gradients and a `log2` for LOD,
then two `sample_level` calls, each walking a Morton address, decoding
DXT1/DXT3 by hand and doing its own bilinear — up to **eight software texel
fetches** where hardware does one on dedicated silicon with a texture cache.

That is why the drain is expensive. `sync` waits on `waitUntilCompleted`, so
its 6.94–9 ms per flip IS GPU execution time — for a **640×480** scene on an
M1 Max, which should be comfortably under a millisecond. The drain is not
mainly a presenter problem; it is the GPU genuinely taking that long.

Early-Z compounds it and the code says so: with `[[early_fragment_tests]]`
off, "every occluded fragment in the scene pays the whole combiner chain and
up to four texture samples first". The run reads
`depth test before the shader: 0 draws early, 7807313 late (early_z OFF)`,
and an `fs_hw_early` variant already exists, gated.

**The change:** upload guest textures as `MTLTexture` with native BC1/BC2/BC3
(Apple Silicon supports them), de-swizzle once at upload — 50,341 uploads
against 13.1 M sample requests, so the cost moves to the right side — and
sample through a real `sampler` carrying the guest's filter and wrap state.
Early-Z then becomes viable for the draws that cannot discard.

This supersedes the earlier framing that direct GPU presentation was the
largest opportunity. Presentation removes readback, snap, convert and upload;
it leaves the drain, and the drain is the GPU doing software texture
filtering. Fix the sampling first — it needs no presenter rewrite.

Where the drain is, measured over a 1167 s player session (49,889 flips),
`progress/CLAUDE_PROGRESS_2026-09-21_NIGHT5_THE_STEAL_PUT_THE_POISON_BACK.md`:

| caller | calls | wait | readback | per flip |
|---|---|---|---|---|
| **external** | 50,121 | **346.4 s** | 30.2 s | **6.94 ms** |
| swap | 100,075 | 33.6 s | 29.3 s | 0.67 ms |
| invalidate | 441 | 0.0 s | 0.4 s | ~0 |

**`external` is 91% of all drain waiting** — one call per flip, the flip
readback, not the diagnostics. Swaps drain twice as often and ten times more
cheaply, because by then the GPU has usually caught up; the flip readback
drains immediately after the frame's work was submitted. The flip round trip
— drain, copy the colour surface to guest RAM, upload it back to present — is
the target, not the swap. `no_flip_sync=1` is not the fix and
`nv2a_pb_exec.c` says why: it presents stale guest RAM.

An earlier revision of this file attributed 443,738 ms of drain and
125,914 ms of readback to a `clear_surface` caller. **There is no such
caller** — the instrument has four (`SYNC_WHO_EXTERNAL`, `SWAP`,
`INVALIDATE`, `FRAME_END`, `nv2a_metal.m:2603`) — and the figure predates the
current naming. `RECOMP_METAL_BATCH` remains the pipelining lever; it is on by
default and measured, and what it lacks is a stability verdict.

Separately: **NV2A vertex programs run on the GPU.** `vsh_gpu_on()`
(`nv2a_metal.m:971`) defaults to 1, and the session above reads
`[METAL] vsh: guest programs on the GPU (metal_vsh on)` with
`vsh draws: 7646170 GPU, 161143 CPU` — 98% of draws, 33 programs compiled
against 7,646,746 cache hits. The CPU interpreter is the fallback and the
control arm. *(This paragraph previously said the opposite — that the MSL
emitter "is not wired into the renderer". It is, and has been by default.)*

What is still on the CPU is vertex **marshalling**, and as of 21 Sep 2026
night it is measured rather than inferred. Two scene-matched `measure.sh` runs
(Corn tutorial, `nodes=61`, 0 guest faults, 59.9 and 60.2 fps) with
`RECOMP_METAL_CB_STATS=1` split a 19.46 ms frame at 66 draws:

| | ms/frame | % frame | µs/draw |
|---|---|---|---|
| `sync` — the GPU drain | 8.95 | 46% | — |
| **our own draw code** | **6.03** | **31%** | **91** |
| Metal create+encode+commit | 0.127 | 0.7% | 1.9 |

**Metal is 0.7% of the frame.** Every "make the Metal calls cheaper" idea is
aimed at nothing. Inside our 91 µs per draw:

| region | µs/draw |
|---|---|
| `prepare_vertices` (the `vsh` stage) | **47** |
| setup + ring reserve + vertex pack | 16.2 |
| surface + texture + pipeline state | 14.7 |
| validation + triangle assembly | 8.8 |

`prepare_vertices` alone is half the per-draw cost. **It has no hot spot.**
`RECOMP_VSH_SPLIT` measured it at **13,615 vertices/frame, 212 ns each**, of
which attribute fetch is only 39 ns (18%); the other 82% is loop overhead
spread across the per-vertex body. `execute` reads 0.0 ms over 0 vertices,
confirming the GPU takes 100% of the programs.

The one candidate that looked obvious — the per-vertex 256-byte seed copy —
was tried and **measured null**: `RECOMP_VSH_HOIST_INPUTS` hoists it to once
per batch (provably equivalent; `s_vsh.current` is loop-invariant and
`fetch_vertex` never touches it) and a scene-matched A/B moved vsh
2.88 → 2.80 ms/frame, 0.4% of frame, inside variance. The switch is kept as a
control arm so nobody retries it.

So this stage does not have a copy to remove; it has **13,615 CPU iterations
per frame to stop doing**, which means uploading the guest vertex buffer and
letting the GPU fetch its own attributes. That is the "everything on the GPU"
endgame and it is a project, not a patch.

Three things were checked and ruled OUT before this split was taken, each of
which looked like the answer: Metal batching is coalescing (17.8 draws/flush
here, ~183 in a heavy scene), the vertex upload already narrows to
`vsh_active->nattrs`, and CPU triangle culling is already skipped whenever
`vsh_gpu_culling` is on, which it is.

**Scene caveat:** absolute µs/draw is scene-dependent — the tutorial runs 66
draws/frame at 91 µs, a heavy street scene 549 at ~42 µs. The proportions are
what transfer, not the absolute figure.

**The ADX priority lock — fixed on the night of 21 September, NOT yet
validated in play.** CRI's ADX lock is mutual exclusion built on priority
elevation, which this runtime records without enacting, so `adx_guard`
restores it with a host mutex spanning the guest critical section
(`RECOMP_ADX_SERIALIZE=1`). That guard had a hole: its 5000 ms steal let a
second thread into the region while the holder was inside, which is precisely
the interleave it exists to prevent. Player session 10 hit it at Beat's race
challenge — `saved_priority` 1 → 15, 0.4 fps for the last 171 s of a 1167 s
run, one 5000 ms frame per five-second window.

**VALIDATED IN PLAY, 22 Sep 2026.** A 2040 s player session ran to a crash
from an unrelated fault with the guard perfectly balanced throughout:
`locks=357285 unlocks=357285 matched (+0 UNMATCHED, 0 SKIPPED)`, `held=0`,
**271 contended acquisitions, 0 stalls, 0 steals, 0 poison**. Sessions 11 and
12 had both wedged by ~620 s; this one ran more than three times longer under
three times the contention and never stalled. Log
`last-run-2026-09-22-SESSION14-CRASH-AFTER-COPS.log`.

The history below is kept because the intermediate state is the interesting
part. Both steal paths are closed and unit-tested with a positive control, and
the poisoning has not recurred. **The fix did first freeze two player sessions
hard at ~620 s** — `saved_priority` stayed healthy at 1 through 169
contended acquisitions, yet the main thread parked in `adx_guard_lock_enter`
for good while the ADX spinner burned a core, with no thread anywhere inside
the guest critical section. It traded a 0.4 fps crawl for a deadlock and the
player got less playtime than before.

The host-thread-migration hypothesis for those freezes was **refuted** from
the logs (`+0 UNMATCHED` across 477,689 locks in three sessions, so lock and
unlock always pair on the same host thread), and the 22 Sep session then
declined to reproduce the freeze at all. The guard now also names its holder
by `pthread_threadid_np` when it stalls, so a recurrence identifies the
holding thread directly instead of by inference from a sample.
Full account, with samples, in
`progress/CLAUDE_PROGRESS_2026-09-21_NIGHT5_THE_STEAL_PUT_THE_POISON_BACK.md`.

**Intermittent crash — now attributed, and the rate was overstated here.**
Across 390 recorded runs, 46 end in a guest fault (11.8%), and the hazard is
not spread through the run: it is concentrated at **t = 43-50 s**, the title
menu to first mission transition. Among runs that reach that transition, 7 of
97 crash; among runs that survive past it, about 1.6%. An earlier version of
this page said "one run in five", which does not match the data.

*2026-09-15: it is ONE INSTRUCTION.* All ten faults across today's scripted
runs share a PC and an operand — `sub_001A2E2E +0x670`, faulting on guest
`0xFFFFFFBE` — and `0x1A2E2E` sits inside the DSOUND section, so this is
recompiled DirectSound rather than anything of ours. `sub_001A2E2E` reads the
guest's software previous/next links and updates the hardware voice list, which
puts the crash in the same correspondence as the trap storm; `0xFFFF` is that
list's own terminator and `0xFFFFFFBE` is sentinel-shaped rather than
NULL-shaped. Every crashing run had also stalled its USB driver and never left
the title screen, so the t=43-50 s concentration above may be describing the
same runs from a different angle. See
`progress/CLAUDE_PROGRESS_2026-09-15_THE_CRASH_IS_ONE_INSTRUCTION.md`.

Of the 33 genuine faults on this host (excluding a known mem-watch build and
some Windows ones), **20 are the same bug**: the DirectSound APU interrupt
handler is entered for an idle-voice trap and dereferences a voice object that
is NULL. The faulting instruction, the guest register fingerprint, the frame
depth and a three-deep call chain read out of the guest's own stack all agree,
and the handler is the one the title registers with
`KeConnectInterrupt(routine=0x001A2681, vector=5)`. What is not yet proven is
which route sets the trap method to the idle-voice value; that needs a
read-only probe at the handler's entry rather than more static reading.

**Intro card transitions.** The fade between the opening cards renders as a cut.
Localised on 14 Sep: the guest computes the ramp, and the vertex buffer it
draws from already contains alpha 255 by the time the renderer sees it, so the
value is lost in recompiled guest code rather than in the renderer. Whether the
same path carries other tints in the game is **not established**, and if it
does this matters well beyond the intro.

**Metal command-buffer batching** is implemented, measured, and **ON by
default — which is not what this file, the code comment above it, or the
decision that followed the SEGA-screen report all say.** Replaying one
captured 492-draw frame through both submission paths, 25 alternating trials:
69.2 ms to GPU completion per-draw against 46.5 ms batched, distributions not
overlapping. It was made the default, a person playing interactively got stuck
on the SEGA screen, twelve scripted boots could not reproduce it, and the
stated resolution on 14 Sep 2026 was to revert to opt-in until understood.

**That revert never reached the code.** `batch_on()` (`nv2a_metal.m:197`)
reads `on = (e && *e) ? (atoi(e)!=0) : 1` — unset means on. The comment
directly above it still says "the default goes back to off". No player
`paths.conf` sets the switch, so every session since has run batched: all five
hung or frozen player sessions of 21 Sep log
`[METAL] one encoder per batch: yes (metal_batch on)`.

This is not a claim that batching causes them — session 10's hang is fully
attributed to the ADX poisoning below, and sessions 3, 5, 8 and 9 show zero
ADX steals, so they are a separate and still-unexplained mode. It is a claim
that **the configuration everyone believed was disabled has been live the
whole time**, and that `RECOMP_METAL_BATCH=0` is now an untried A/B against
those four. Decide the default deliberately; do not leave three documents
disagreeing with one line of code.

**Vertex reuse** is implemented and off. Its correctness gate is unresolved: one
unexplained output mismatch in around 225M shader invocations.

## Recently fixed

The controller used to die partway through most sessions. The cause was a
guest store that never faulted: the MCPX register page had to be made writable
for the trap handler to perform a store, and a guest store landing inside that
window completed as plain memory with none of the register semantics the trap
exists to supply. The guest re-arms its master interrupt enable constantly, and
that write landing untrapped replaced `HcInterruptEnable` wholesale, taking
`WritebackDoneHead` with it — after which the driver was never told its done
queue had been published, never claimed it, and never queued another transfer.

The aperture is now mapped twice: the guest's view, guarded and never
unprotected, and a private always-writable alias the runtime writes through.
Ten of twelve runs froze before; none of seven since.

Full account: `docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-14_USB_STALL.md`.
It separates what is verified from what is still a hypothesis — the bypass is
demonstrated and its removal coincides with healthy runs, but no single run
shows the bypass followed by the freeze.

## Open, and individually tractable

- Why `clear_surface` costs 11–15 ms a frame, and whether the GPU wait inside
  it is avoidable. It is where the stall is *paid*; what *creates* it is the
  draws.
- The intermittent guest fault: confirm, with a probe rather than by reading,
  that the idle-voice trap is being delivered for a torn-down voice.
- Where the intro fade is lost in guest code, and whether that path is shared.
- The vertex-reuse mismatch.
- Whether batching can be made default-safe.
- Whether batching can be made default-safe — the last open question on the
  biggest measured rendering win.
- The voice-list ownership defect underneath the trap storm. The guest owns the
  list and the head; our `VOICE_ON` writes both underneath it, and `regs[top]`
  goes stale, so a re-ON links a voice to itself. Mitigated, not cured: the two
  APU changes below restore the engine's frames without fixing why `TVL` is
  stale. `docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-15_VOICE_LIST_SELF_LINK.md`.

## Fixed 15 September 2026 — the sound engine

It was running on 63.6% of APU frames and now runs on 98.6%, measured at a
scene-verified mission. `trapped=` was never a count of traps raised; it counts
frames where the engine is **switched off** because the front end is in TRAPPED
state, so the old number measured how long it spent waiting for each trap to be
serviced.

Two changes, each measured over two matched pairs before either shipped:

| | engine duty | traps raised |
|---|---|---|
| before | 63.8% / 63.5% | ~10,300 |
| `SE_WHILE_TRAPPED` alone | 99.1% / 98.8% | ~125,000 |
| both, shipped | 98.8% / 98.6% | ~10,800 |

The engine had been throttling its own trap rate by switching itself off, which
is why they only make sense together: coalescing a trap that is already
outstanding removed 92% of the raises with duty unchanged. That matters because
each raise is a guest interrupt into the DirectSound ISR where 20 of the 33
recorded faults land.

**The gate on this is still open and is not a measurement.** The switch carried
the condition "no default until it has been heard at gameplay, with a
controller". A person still has to listen. `RECOMP_APU_SE_WHILE_TRAPPED=0`.
