# JSRF goals — the title runs; the pushbuffer handshake is the frontier, 5 September 2026

Supersedes the active goals in `JSRF_GOALS_2026-09-04_AUDIO_STALL.md`, which
stays as the record of how the audio stall and the memory corruption were
found. Every claim below was measured in a running process.

## Where we are

The title no longer crashes and no longer waits. It boots, loads, plays its
BGM through the CRI stack, runs the title screen's state machine to
completion, tears it down, and builds a successor scene of 128 objects that
the update walker services continuously. Four consecutive 100-second runs, no
fault.

What it does not do is finish a frame. The main thread spends **2485 of 2485**
samples in one loop — D3D's pushbuffer free-space wait — and the grant that
releases that loop arrives about **four times a second**. Nothing reaches the
screen because nothing completes.

So the frontier has moved twice in one day: from "it faults" to "it renders
nothing" to "it renders at four hertz". The remaining problem is a *rate*, and
it is in our runtime, not in the title.

## Closed by measurement, 5 September

Settled. Do not re-open without new runtime evidence.

- **G14, G19** — the ADX/WXCI stall and the file-I/O APC ordering. Closed 4
  September, still holding.
- **G20 — the post-BGM corruption.** An unresolved `_flags` fallback at
  0x00014885 disarmed an empty-list guard; the array-remove loop behind it ran
  with `count - 1` underflowed and overwrote the kernel import thunk table.
  Fixed by `backport_empty_list_guard.py`. See commit 59bf975.
- **G15 — what the title waits for.** Nothing. Global object id 8 walks state
  0x12 → 0x13 → 0x14 in three consecutive visits and is never visited again;
  state 20 is the terminal teardown and completing it is what that looks like.
  Commit 914c76c.
- **G22's first half — the renderer.** Not a transform problem and not a
  draw-path problem. The vertex shader runs (batches 9915, rejected 0), 34,504
  triangles rasterise, **zero** off-surface. The `[GPU] draws ... input x
  -4636..4636` range is the *pre-shader* range by construction and says nothing
  about the transform; it was cited as evidence of a broken transform and that
  was wrong.
- **The pushbuffer ring desync.** On a backwards PUT with no jump at the
  cursor the feed parsed the ring's stale tail as commands, landed on last
  lap's ARRAY_ELEMENT16 index pairs, and set the permanent `stream_fault`.
  Fixed; invalid headers per run 1+ → 0. Commit a54ff90.
- **Dead `_flags` fallbacks in reachable code.** Were 2, now 0. The ratchet
  stands at 78 latent sites; none of them execute.

## Goals, in order

### G23 — ACTIVE. Explain four pushbuffer grants a second

The main thread spins in `sub_00191440` at `loc_001914F0`:

```
ecx = [edx]        ; GET
esi = edi - ecx    ; outstanding = PUT - GET
cmp eax, esi
jb  loc_001914F0   ; spin while needed < outstanding
```

`RECOMP_PB_WAIT_TRACE=1` shows it is **not** deadlocked: GET advances, and
`outstanding` sits at 6 against a `needed` of 2. But across 40 million spin
iterations in 100 seconds it advances roughly four times a second.

GET is advanced by our own `jsrf_pushbuffer_ack` thread, in
`diagnostics/jsrf_first_fault/main.c`, whose body is

```
consumed = jsrf_pb_poll();
if (getp && consumed && MEM32(getp)!=submitted) MEM32(getp)=submitted;
Sleep(0);
```

A `Sleep(0)` loop should acknowledge thousands of times a second. Two
explanations, and they need opposite fixes:

1. **The loop runs fast and the condition is wrong** — `consumed` is usually
   false, or `MEM32(getp)==submitted` already, so the ack is skipped.
2. **The loop barely runs** — the thread is starved (see G24).

**First step:** count the ack thread's own iterations and how many of them
take each exit, and report the rate. That is one counter and one line; it
distinguishes the two without guessing.

**Acceptance:** a measured statement of which of the two it is, and the grant
rate in the hundreds per second rather than four.

### G24 — `nv2a_ack_thread` spends its life in `mprotect`

In the same sample it spends ~37% of its samples in `__mprotect`, re-arming
the MCPX write trap, and another ~37% in `cthread_yield`. `mprotect` takes the
process-wide VM lock on macOS, and the main thread is spinning flat out on
another core throughout.

Re-arming should be driven by the guest actually touching the guarded page,
not by a loop. This is the most likely cause of G23's explanation 2, and it is
independently worth fixing.

**Acceptance:** `mprotect` falls out of the thread's profile, and the main
thread's tick rate improves measurably.

### G16 — Guest thread priorities are not honoured

`sub_0013B180` is CRI's idle/CPU-load thread: by design it runs at the lowest
priority and soaks up slack. Here it burns a full core. G23 and G24 are the
same subject reached from the pushbuffer side; fix those first and re-measure
before doing scheduler work.

**Acceptance:** the idle thread yields to higher-priority guest threads.

### G25 — Two unfinished edges of the pushbuffer parser

Neither is currently reached, both are real.

- **NV2A `call` headers** (`(word & 3) == 2`) are refused by design — "calls
  and returns need a caller-owned stack". The hardware subroutine is one deep;
  implementing it is `NV2A_PUSHER_CALL` plus a return address in the feed loop,
  with a case in `jsrf_pusher_stream_test`.
- **`stream_fault` is permanent.** One unparseable stream disables rendering
  for the life of the process. It should be recoverable — resynchronise at the
  next published PUT rather than never again.

### G21 — Carry lifter flag state across fallthrough boundaries

`_fa`/`_fb`/`_fas`/`_fbs` are locals of each generated function, so a branch in
a fallthrough continuation loses the flags its `cmp` computed. 78 sites remain
in 49 functions; **none execute today**, so this is correctness for the future
rather than a live defect. Best done alongside a regeneration.

**Acceptance:** a lifter change plus its unit test, and the ratchet's dead
count falls.

### G17 — The ADX server thread is vblank-paced at ~30 Hz

`sub_0013B1C0` spends almost all its samples in
`D3DDevice_BlockUntilVerticalBlank`. Hardware gives 60.

### G18 — Close the 0x0013F900 undetected entry point

A real gap in the dispatch table, and harmless: the target is `eb fe`, CRI's
deliberate halt stub. `jsrf_cri_handler_probe` now reports whenever the
middleware dispatches through it.

### G5 — Host input, no longer a blocker

The premise is disproven twice over: the title was never waiting for a button,
and it is no longer waiting at all. Still work worth doing; re-scope it from
the first screen that actually reads a pad.

## Readiness

Not playable. But the failure mode is now a performance defect in our own
runtime rather than a crash, a stall, or a missing feature in the title, and it
has a number attached to it: four pushbuffer grants a second, against the
hundreds a frame needs.

## Non-goals for now

No Windows/D3D11 build. No full JSRF regeneration. No upstream merge. No Metal
or GPTK renderer. Playable gameplay is a later milestone with no date.
