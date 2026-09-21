# Where Jet Set Radio Future has got to

Last measured 14 September 2026, against the tree at `a113ae9`, title built
`-O2`, on an Apple M1 Max, with the 21 September section below added against
`49ff7e2`. Every number here came from a run; where something is believed
rather than measured it says so.

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
holds 60.1 fps in xemu. The `clear_surface` cost is now split: over a 20-minute
play session, 443,738 ms went on draining the GPU and 125,914 ms on reading back
and converting — **78% drain, 22% readback**. The code's own note says what that
means: drain-dominated leaves pipelining as the only win, and a resident clear
could remove the readback half only. `RECOMP_METAL_BATCH` is the pipelining
lever and is measured; what it lacks is a stability verdict, not performance
evidence.

Separately, and larger: **NV2A vertex programs are interpreted on the CPU on
macOS** (`nv2a_vsh_execute`, ~7.5 ms of a ~32 ms frame). The D3D11 path emits
HLSL instead. An MSL emitter now exists with full opcode coverage and tests, but
it is not wired into the renderer and must not be until its outputs are compared
against the interpreter — moving vertex work to the GPU also moves triangle
assembly, culling and the reject paths, which all currently read the shader's
CPU-side output.

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

**Metal command-buffer batching** is implemented, measured and **off by
default**. Replaying one captured 492-draw frame through both submission paths,
25 alternating trials: 69.2 ms to GPU completion per-draw against 46.5 ms
batched, distributions not overlapping. It was briefly the default and a person
playing interactively got stuck on the SEGA screen; twelve scripted boots could
not reproduce that, and it is opt-in until it is understood. A measured
rendering win does not outrank a title that will not start.
`RECOMP_METAL_BATCH=1`.

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
