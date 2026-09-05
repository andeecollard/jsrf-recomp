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
samples in D3D's pushbuffer free-space wait, because GET stops advancing —
and GET stops advancing because our push-buffer parse desynchronises exactly
once per run and a static `stream_fault` makes that permanent.

So the frontier moved three times in one day: from "it faults" to "it renders
nothing" to "it renders at four hertz" to **one bad header in six and a half
million dwords**. Everything else has been measured and ruled out.

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
- **Missing PFIFO control flow.** CALL and RETURN were unimplemented and both
  read as malformed headers. Implemented and unit-tested in f03d3a1 — and JSRF
  executes neither, so it was a real gap and not the cause.
- *Not* closed: the ring desync. A "backwards PUT with no jump means a restart"
  branch in a54ff90 appeared to fix it, and was reverted in f03d3a1 because it
  was a heuristic that hid the event and may have caused desyncs of its own.
  See G26.
- **Dead `_flags` fallbacks in reachable code.** Were 2, now 0. The ratchet
  stands at 78 latent sites; none of them execute.

## Goals, in order

### G23 — CLOSED. The rate is a latch, not starvation

Answered by counting the acknowledgement thread's own loop and its exits:

    [PB-ACK] 3422711 loops/s: acked=4383 already=950582
             not-consumed=298112789 no-device=28533

The thread runs **three and a half million times a second** -- it is not
starved, and G24 is not the explanation. `acked` freezes at 4383 and every
subsequent loop takes the "not consumed" exit, three hundred million of them in
one run, because `jsrf_pb_poll` latches a static `stream_fault` on its first
parse failure and returns 0 for the rest of the process. GET then never
advances and the title spins in its pushbuffer reserve forever.

So four grants a second was one parse failure followed by silence, not a slow
handshake.

### G26 — ACTIVE. One desync per run, at a backwards PUT to a non-base address

This is the root cause; everything else was downstream of it.

The parse is almost perfect: **bad_headers=1 across 6,406,120 dwords**. The
segment history places that single failure exactly:

    seg 3557: from=005B96F8 end=005E96F8 put=005E96F8 stop=0 consumed=49152
    seg 3558: from=005E96F8 end=005ED000 put=005E6C80 stop=3 consumed=1

One dword past a window consumed cleanly to its end, on a poll where PUT had
moved **backwards, to an address that is not the ring base**. Every healthy
wrap in the same run does the opposite -- PUT becomes the ring base and a JUMP
is waiting in the tail (segs 3544, 3547, 3550, 3554, all stop=2).

The first question is whether the guest really published that value or whether
we read it wrongly, and one instrument settles it: record every backwards
transition of PUT with the previous value, the cursor, and an immediate
re-read. A value that changes on re-read is a race; a stable one is the guest.

Do not resynchronise, skip, or scan to get past this. Two heuristics of mine
were reverted for exactly that reason (commit f03d3a1) -- one of them may have
been producing desyncs of its own.

**Acceptance:** a measured statement of what writes that value and why, and
bad_headers 0 over a full run.

### G25 — CLOSED for CALL/RETURN; `stream_fault` deliberately unchanged

PFIFO's control flow was a stub and the code said so. CALL ((h & 3) == 2) fell
through to INVALID, and so did RETURN, because 0x00020000 masks to 0x00020000
under 0xE0030003 and matches neither method form -- a legal return read as a
malformed header.

Both are now implemented with hardware semantics: CALL reports its target and
the caller saves the cursor after the call word, which is the DMA_GET hardware
saves; RETURN restores it. One deep, as PFIFO is. `jsrf_pusher_stream` covers
both. Commit f03d3a1.

**And JSRF uses neither.** Zero CALLs and zero RETURNs in 110 seconds with the
opcodes implemented, so the (h & 3) == 2 words seen in earlier runs were data,
not calls. A real gap, closed, and not the cause.

`stream_fault` stays permanent on purpose until G26 is understood. Making it
recoverable would hide the one event per run that matters.

### G24 — `nv2a_ack_thread` spends its life in `mprotect`

It spends ~37% of its samples in `__mprotect`, re-arming the MCPX write trap,
and another ~37% in `cthread_yield`. `mprotect` takes the process-wide VM lock
on macOS.

**Demoted:** this was proposed as the explanation for G23 and measured not to
be -- the acknowledgement thread runs three and a half million times a second.
It remains real waste and worth fixing, but it is not blocking anything.

**Acceptance:** `mprotect` falls out of the thread's profile.

### G16 — Guest thread priorities are not honoured

`sub_0013B180` is CRI's idle/CPU-load thread: by design it runs at the lowest
priority and soaks up slack. Here it burns a full core. G23 and G24 are the
same subject reached from the pushbuffer side; fix those first and re-measure
before doing scheduler work.

**Acceptance:** the idle thread yields to higher-priority guest threads.

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

Not playable. The failure mode is a single push-buffer parse desynchronisation
per run -- one bad header in six and a half million dwords -- which a permanent
`stream_fault` then turns into a dead renderer and a title spinning forever in
its reserve loop. Everything else that looked like the problem (the transform,
the draw path, thread starvation, missing PFIFO control flow) has been measured
and ruled out. G26 is the whole of it.

## Non-goals for now

No Windows/D3D11 build. No full JSRF regeneration. No upstream merge. No Metal
or GPTK renderer. Playable gameplay is a later milestone with no date.
