# Where Jet Set Radio Future has got to

Last measured 14 September 2026, against the tree at `a113ae9`, title built
`-O2`, on an Apple M1 Max. Every number here came from a run; where something
is believed rather than measured it says so.

## Working

| | evidence |
|---|---|
| Boots to gameplay unattended | 12 of 12 scripted boots reached at least the title gate (`NtOpenFile` 1342); most reach New Game (1408) |
| Renders | 245,331 native draw batches in a 75 s intro run, 0 software fallbacks |
| Audio | output holds 47,602–48,006 Hz across every scene measured, 14 runs |
| Controller input | 7 of 7 full 300 s runs retire USB transfers continuously; the guest's own driver acknowledges ~31,000 done queues per run |
| Tutorial | completes; the title reaches the playable part |
| Tests | 28/30 C tests, 44/44 recompiler Python tests |

The two failing C tests fail deliberately. They are site-specific gates on two
unresolved lifter defects and are documented as such in `CMakeLists.txt`; a
green suite there would mean the gate had stopped working.

## Not working

**Frame rate.** 26–31 fps at gameplay against a title that holds 60.1 fps in
xemu. Roughly a third of the frame is `clear_surface`, 11–15 ms across two
calls, most of it waiting on the GPU. Measured, not yet addressed.

**Intermittent crash.** Around one 300 s run in five ends in a guest fault with
the stack pointer outside the primary stack. Long-standing, unattributed.

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
- The intermittent guest fault.
- Where the intro fade is lost in guest code, and whether that path is shared.
- The vertex-reuse mismatch.
- Whether batching can be made default-safe.
- One scene traps the APU on more than half its frames and empties the output
  queue. Audio keeps flowing, so this is a robustness question, not a silence.
