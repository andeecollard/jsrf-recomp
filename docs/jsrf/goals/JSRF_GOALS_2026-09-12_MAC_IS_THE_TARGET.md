# Goals — 12 Sep 2026: the Mac is the target, Windows is an instrument

Written after an upstream-parity comparison and a day of measurement on both
hosts. It exists to answer one question that was about to send a lot of work in
the wrong direction: *does a Mac decomp/recomp need the Windows build debugged
first?*

No. Windows is an oracle, not a gate. The reasoning is below, and the goals are
ordered by what actually stands between here and a Mac build someone can play.

## 0. The premise, checked

Measured today, same machine, same title, same anchor:

| | macOS / Metal | Windows / D3D11 (CrossOver) |
|---|---|---|
| to gameplay | ~30 s | ~150 s |
| batches/second | ~4,720 | ~500 |
| guest clock 1000 | 13 s | 14 s |
| controller | works — a human played to the tutorial | pad never reaches the guest |
| renderer | GPU | GPU, since today |

The Windows build is not a second platform we owe a port to. It runs under
CrossOver **on this same Mac**, so its throughput is not a property of the
recompiler at all, and `jsrf-compare-hosts-by-loop-count` already records that
comparing the two by wall clock produced an entire fake audio investigation.

Windows has earned its keep exactly once, and it is worth being precise about
how: it made the black frame reproducible under a different renderer, which is
what exposed the CPU clear overwriting guest RAM after the draws had been
written back into it. That is a *renderer model* bug, and the finding transfers
to Metal. Nothing about Windows **input** transfers, because macOS input works.

So: keep Windows building and comparable. Do not chase its OHCI enumeration or
its frame rate.

## 1. The only thing between here and a playable Mac build

A human reached the Corn tutorial with a working pad and stopped at
*"Press the 'A button' and jump 1 time!"*. That is the project's oldest open
item and everything cheap around it has already been eliminated:

- object state at the tutorial matches xemu exactly — 61/61, 47/47, identical
  id sets
- the draw path is eliminated — lists and eACTFLAG filters match xemu
- half the historical "input stall" sightings were a synthetic START pausing
  the game, which is why `RECOMP_FAKE_PAD=a` exists and never sends START

Two live leads remain: a 17x update asymmetry between the two CPlayers, and the
CPlayer animation gate — Corn is object id 45, and the gap is 330 structured
dwords of bone data.

**G1.1** Split input from animation. `RECOMP_PAD_TRACE` is edge-triggered and
settles it without a human in the loop: press A at the prompt and either the
edge appears (input is fine — it is the animation gate) or it does not (A is
not arriving, which is a mapping problem and much smaller). `nonneutral` cannot
answer this; it counts polls, so a flat counter and nobody pressing look the
same.

**G1.2** If it is the gate: find the writer of the 330 dwords. Unattended
sampling rather than probes — `jsrf-instrumentation-weight-destabilises-jsrf`
says heavy probes trigger the poll stall and then read as a controller fault.

**Done when:** the tutorial advances past the jump on macOS with a real pad.

## 2. Renderer residency, finished

Today's ownership seam works and is proved by test, but the measurement said
plainly that it has never fired in JSRF at the clock-1000 anchor, and the
negative control (`RECOMP_GPU_OWN=0`) reached the anchor with byte-identical
counters. It *does* fire in gameplay — 20,480 real hits — so it is load-bearing
past the title screen, not before it.

**G2.1** Give the framebuffer presenter a `sync_range`. `fb_present.c` reads
guest RAM directly from `nv2a_pb_exec_surface()` with no sync call, which is
why `RECOMP_D3D11_RESIDENT_CLEARS=1` bands the live window: the GPU-only
regions never reach it. This is the single fix that makes resident clears
usable outside a flip capture.

**G2.2** Then let the flip consume the retained target instead of downloading
it, and bind render-target-as-texture as an SRV rather than round-tripping
through guest RAM.

**Done when:** resident clears are on by default on both hosts with a clean
live window, and the readback count at clock 1000 is materially below 1,986.

## 3. Windows, scoped

**G3.1** Keep it building and keep the two hosts comparable at equal guest
loop count. That is its whole job.

**G3.2** Do **not** work on: OHCI enumeration (it stalls the guest at one
batch and buys nothing for the Mac), CrossOver frame rate, or Windows-only
input. If Windows input is ever wanted, the Burnout 3 model is the cheap one —
that project has no USB emulation at all, 127 lines of host XInput read
straight in `main.c` — but it does not transplant while JSRF's XAPI is
unidentified in the decompilation.

## 4. Upstream hygiene — a class of bug, not an incident

Merge `36b4076` resolved `kernel_rtl.c` in favour of the JSRF side and silently
dropped upstream v0.8.0's critical-section work. `kernel.h` kept the
declaration of `xbox_SetCrtLockTable` pointing at an implementation that no
longer existed, and that dangling declaration was the only trace. Restored in
`6f5a19e`.

**G4.1** Add a merge guard: a test that fails when a symbol present in
`upstream/main` has vanished from our tree without an explicit waiver. The
comparison that found this was manual and took an afternoon; it should be a
command.

**G4.2** Re-run the full upstream comparison after every `git merge upstream`,
not once a quarter.

## 5. Decompilation — a different project, named honestly

"Recomp" and "decomp" are being used interchangeably and should not be.

- **Recomp** is what exists: a mechanical x86→C lift that runs. It is close to
  playable and goals 1–2 finish it.
- **Decomp** is named, readable, buildable C that a person can edit. That is a
  multi-year effort of a different kind.

The seeds are real: upstream's `tools.split` emits one byte-exact `.s` per
function (2,254 of 2,254 verified on the Dashboard), and a sibling checkout has
1,332 named JSRF functions at 99% alignment to our VA space — with no licence,
so it is a reference and must not be vendored.

**G5.1** Do not start the decomp track until goal 1 is done. A playable recomp
is the thing that makes a decomp checkable, because it gives every renamed
function a behavioural oracle.

## What this replaces

Supersedes the Windows-first framing in
`handovers/CLAUDE_TO_CODEX_HANDOVER_2026-09-12_WINDOWS_PLAYABLE.txt`, which is
still correct about Windows' *state* and wrong only about its priority.
