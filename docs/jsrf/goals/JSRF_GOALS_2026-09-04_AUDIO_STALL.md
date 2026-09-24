# JSRF goals — the loading stall is the audio path, 4 September 2026

> Superseded, 5 September 2026, by
> `JSRF_GOALS_2026-09-05_PUSHBUFFER_RATE.md`. Kept for the record; the active list is
> the newest goals file in this folder.

> **Superseded by `JSRF_GOALS_2026-09-05_PUSHBUFFER_RATE.md`.** Kept as the
> record of how the audio stall, the empty-list guard and the render
> frontier were found. G14, G15, G19 and G20 are closed here.

Supersedes the active goals in `JSRF_GOALS_2026-09-03_RENDERER.md`. Evidence is
in `CLAUDE_HANDOVER_2026-09-04_STALL_FOUND.txt`; every claim below was measured
in a running process, not read out of the disassembly.

## Where we are

The "Now Loading" stall was found and its WXCI completion defect is now fixed.
The title BGM reaches the input ring buffer and begins decoding; the active
frontier is the newly exposed post-BGM pointer corruption in G20.

It was not slowness. The root per-frame service ticked ~620/s with the CPU
rasteriser and ~5,100/s without — 481,812 ticks in 94 seconds — without the
state machine advancing one step. It was stuck, not starved.

It is not input. G5 promoted "real host input to the guest" to the blocker on
the reading that the title "animates while four healthy threads wait for a
button press". Measured: the title-screen object (global id 8, `0x00E20870`) is
in state 0x14 of a 21-entry jump table, and that state waits for global object
id 2 to be destroyed — not for a button. The wait chain is fully traced.

It is the audio path, which this file's predecessor listed under **non-goals**
("Sound stays bypassed"). That non-goal turns out to be load-bearing: the title
screen waits for its own BGM to buffer.

The full chain, each link measured, top to bottom:

| # | What waits | On what |
|---|---|---|
| 1 | title screen (id 8), state 0x14 | global object id 2 to be destroyed |
| 2 | id 2 (`0x00E1C170`) | id 5's `+0xBC` to reach 2 |
| 3 | id 5 (`0x00C35CC0`), ADX stream manager | decoder to hold ≥ 0x2000 units |
| 4 | decoder (`0x002771B0`) | filled = 0, capacity 0x4000 |
| 5 | ADXT handle 0 | stuck in PREP |
| 6 | stream (`0x0027C720`), state 1 | ≥ 0x10 bytes from the input ring buffer |
| 7 | input ring buffer (`0x00277180`) | a view-0 commit that never comes |
| 8 | `sub_0013C070`, ADXF read server | its completion poll to return 1 |
| 9 | `sub_001405B0` (wxCiGetStat) | **the WXCI request byte `req+1` to become 1** |

The file itself is fine. `title.adx` is opened, sized and read — `want=51200
got=51200` — and its bytes are in the buffer: `0x00C3DF80` holds a valid stereo
44.1 kHz ADX header. What fails is the bookkeeping.

## Closed by measurement, 4 September

These are settled. Do not re-open them.

- **Tick rate / the CPU rasteriser.** Half a million main-loop iterations, no
  progress. The 233× rasteriser slowdown is real but is not the stall.
- **Suspend/Resume wakeups.** The CRI worker's own counter `0x0025EFB0` runs
  321 → 1588 in 70 s, in lockstep with the server thread's `0x0025EFAC`. The
  self-suspend/resume handshake works.
- **ADX server registration and dispatch.** Gate `0x0022DB20 == 1`, group-2
  slot 0 (`0x00261638`) holds `0x00141F10`, `sub_00141E50` calls it, and a
  sample catches the whole pipeline executing every tick.
- **Pre-BGM unresolved indirect calls.** Zero `[ICALL] Failed to resolve VA` in
  94 s. A distinct post-BGM target is now part of G20.
- **`0x0013F900`.** It is `eb fe`, `jmp $` — CRI's fatal-halt stub, correctly
  never reached. Not a lost producer.
- **The renderer, the heap, thread deadlock, the vblank acknowledge, the flag
  fallbacks, the root object's latch bank, the pushbuffer spin.** All closed by
  the previous session and all consistent with this chain.

## Goals, in order

### G14 — CLOSED by measurement. The acquirer is named; the blocker moved down

Closed on the day it was set. The instrument it called for was built
(`jsrf_ringbuf_probe`, `jsrf_adxf_probe` in `startup_probe.c`, registered
through `instrument_startup.py`; read-only, env-gated, self-limiting) and named
the acquirer immediately:

```
[RINGBUF] call=3 pc=0013F9E0 ret=0013C1C3 obj=00277180 view=0 arg=000D0000
```

`sub_0013C070`, the ADXF read server, asks for the whole 0x000D0000 buffer once
and never commits it. Whole-run tally: 34 view-1 acquires and 31 view-1 releases
from the header read, one view-0 acquire, and **zero view-0 commits**.

Its table entry is alive and serviced every tick — `entry=0027BD20
state=01/02/01 buf=00277180 handle=00261118` — so it is not "never called"; it
is parked on the completion poll at `loc_0013C088`, which needs
`sub_0013DE70(0x00261118) == 1`. That resolves to `handle->vtable[0x2C]`,
measured as **`sub_001405B0`**, which is:

```
wxCiGetStat(req):  if (!req) { report; return 0; }  return (int8_t)req->[1];
```

**At this frontier the whole title screen hung on one byte: the WXCI request
status at `req+1`, `req = MEM32(0x00261118 + 4)`, never became 1.**

### G19 — CLOSED. File-I/O APCs now complete at the alertable wait

The request was parked on status 2. The native read completed successfully,
but the bridge invoked its APC inline before `NtReadFile` returned. WXCI's
request server expects the opposite ordering: it sets `request+0x14C` to 1,
issues the read, observes that flag still set, performs an alertable delay, and
only after the APC clears the flag publishes status 1. Inline delivery cleared
the flag before the first observation, so the server took its early exit and
left status 2 forever.

`kernel_bridge.c` now queues file-I/O APCs per guest thread and drains them only
from alertable waits/delays. The callback's synthetic guest stack is restored
exactly at the bridge boundary. `jsrf_counted_file` covers both properties: a
synchronous read leaves the callback pending, the next alertable delay invokes
it and returns `STATUS_USER_APC`, and the guest ESP is unchanged apart from the
delay's own stdcall frame.

Fresh-HDD runtime evidence (`codex-apc-fix-03`):

```
[WXCI-REQ] pc=00140BDD status=02 issue=0 pending=1
[WXCI-REQ] pc=00140C05 status=02 issue=0 pending=0
[ADXF] req_status=01 io_info=0000C800
[RINGBUF] pc=0013FBC0 view=0 filled=0000C800
```

The reader then consumes its first 0x1000-byte block and the input count remains
non-zero at 0xC7DC. The WXCI/ADXF boundary therefore meets its acceptance test;
the higher title-state chain is interrupted by the newly reachable G20 fault.

### G20 — CLOSED, 5 September. An unresolved-flags fallback, not a bad pointer

The faulting object was never the point. The corruption was a runaway write
loop, and the run now survives its full bounded interval with no fault at all.

**The chain, each link measured.**

`sub_00014870` removes an entry from an 8-slot array at `this+0x70` whose count
lives at `this+0xB0`:

```
00014870  push ecx/ebx/ebp/esi
00014874  mov  esi, ecx            ; this
00014876  mov  eax, [esi+0xB0]     ; count
0001487C  xor  ebp, ebp
0001487E  cmp  eax, ebp            ; count vs 0
00014880  push edi
00014881  mov  [esp+0x10], ebp     ; index = 0
00014885  jbe  0x00014909          ; count == 0 -> do nothing
0001488B  lea  ebx, [esi+0x70]     ; ...otherwise walk the array
```

The recompiler split those ten instructions into three generated functions --
`sub_00014870`, `sub_00014881`, `sub_00014885` -- chained by fallthrough calls.
Guest registers survive that hand-off because they are globals. The lifter's
`_fa`/`_fb` flag snapshot does not: it is a local of each generated function.
So the `jbe` at 0x00014885 fell through to the `_flags` fallback, which is a
constant zero, and **the empty-list guard was never taken**.

The body then ran on an empty list and called `sub_000147A0(this, 0)`, whose
tail-closing loop is `for (i = index; i < count - 1; i++) a[i] = a[i+1]` with
unsigned compares — verified byte-for-byte against the XBE at 0x000147F8, so
the loop itself is lifted correctly. With `count == 0` the bound is
0xFFFFFFFF and it copies every dword down one slot for as far as guest memory
is mapped.

What it reached, in the run that caught it, was the kernel import thunk table
at 0x001C3F60. Every entry ended up holding the *next* entry's synthetic
dispatch address, so the title's `call [0x001C4034]` — `ObReferenceObjectByHandle`
in the thread-priority helper at 0x00147D12, reached from CRI's
`sub_0013B0A0` — arrived at `ExQueryNonVolatileSetting` instead, with
`PsThreadObjectType`'s neighbouring kernel function pointer where the `Type`
argument belonged. The SIGSEGV at host 0x3FE000110 was that bridge storing
through it: guest VA 0xFE000110, a synthetic thunk address, not a pointer.

**How each step was measured, since none of it is visible statically.**

- A guard in `bridge_ExQueryNonVolatileSetting` that rejects unmapped guest VAs
  turned the fault into a report naming the ordinal, the slot, the dispatch
  target and the guest return address, plus the whole call frame.
- The frame matched the call site's pushes exactly except one dword, and the
  dispatch target was one slot high — so the *table*, not the caller.
- `RECOMP_PEEK` on those entries read the correct values; the bridge read them
  shifted. Both use the same memory offset (measured: +12884901888 in each), so
  the contents were changing, not the view.
- `RECOMP_KERNEL_WATCH=0x001C4034` named a change across a blocking wait, which
  names no writer.
- `RECOMP_GUARD_PAGE=0x001C4034` (new, in `main.c`) makes the page read-only
  before the guest runs. The next fault named the writer: `sub_000147A0`.
- `jsrf_list_remove_probe` (new) at 0x000147A0 reported the argument that made
  it run away: `this=0491B4E0 index=0 count=0`.

**The fix.** `backport_empty_list_guard.py` restores the condition at
0x00014885 from the live `eax`, in the same idiom as the FCMOV, flag-merge and
ADX-loop backports this tree already carries, with a `--check` ctest
(`jsrf_empty_list_guard`). ctest is 16/16. The unresolved-flags ratchet drops
from 80 to 79 and its limit was lowered to match.

**The general fix is still owed** — see G21. This is one of 79 sites where a
conditional branch reaches the `_flags` fallback; the others are latent by the
same mechanism.

**What the title does now.** Two 100-second runs, no fault. The live
registered-object count goes 0x14 -> 0x69 -> 0x7F (20 -> 105 -> 127), the
update walker keeps visiting nodes (46,566 visits and still counting at the
end), and the new nodes are `vtable=0x001C4500` objects whose update method is
0x00014870 itself. 15,918 draws are submitted, against 9,400 before.

The framebuffer is blank: `[FB] nonzero=1/153600 same`. The draws carry input
coordinates of x -4636..4636, y -801..4172, which are not clip-space, so the
new content is not being transformed into the viewport. That is a renderer
question, and it is the next frontier rather than a crash.

### G21 — Carry lifter flag state across fallthrough boundaries

`_fa`/`_fb`/`_fas`/`_fbs` are locals of each generated function, so a
conditional branch in a fallthrough continuation loses the flags its `cmp`
computed and takes the constant-zero `_flags` path. G20 is the first time that
has been shown to corrupt memory rather than merely mis-branch.

The audit already counts the sites: 79 dead fallbacks in 50 functions
(je=23, jne=12, jnp=11, jbe=7, jge=5, jae=4, jb=3, jg=3, loopne=3, jle=2,
ja=2, jp=2, jl=1, loop=1). Publishing the snapshot to globals at a fallthrough
hand-off, the way registers already are, would close all of them at once.

**Acceptance:** a lifter change plus its unit test, and the ratchet's dead
count falls. Regenerating JSRF is not required to land the lifter fix; until
it is regenerated, individual sites still need backports.

### G20 (original) — the first post-BGM pointer corruption, as it was framed

Once decoding starts, the title reaches code that the stalled build never did.
Two fresh runs then fault in `sub_00177FE0` (an AddRef) after an invalid dynamic
indirect target. The target is not stable (`0x018042F0` in one run,
`0xFF39FF38` in another), so it must not be registered as a missed XBE entry.
Both runs had already exhausted the 61,800,448-byte guest arena and were seeing
many failed contiguous allocations from return address `0x00199789`; the first
job is to distinguish a failed-allocation consequence from another damaged
return path using the existing heap, stub and ABI instruments.

A second pair of runs (`claude-apccheck-01`, `-02`) found the fault
**deterministic** under different conditions from the two above: identical
registers both times, `EAX=ESI=EF8F8F52 ECX=EBX=042D0E60 EDI=041E3010`, one run
with the ADXF/RINGBUF/startup probes enabled and one with none. So the varying
target seen elsewhere is not intrinsic; there is at least one reproducible
instance, which is much cheaper to chase.

Those runs also name the path. The guest stack at the fault is:

```
0050FEB4 = 00011D42   sub_00011D00, the mode-0 DRAW walker
0050FED0 = 00011D8D   "
0050FF00 = 0001254C   sub_000123E0 loc_0001254C, just past the bank-B dispatch
0050FF18 = 00013B24   sub_00013A80's call into it
```

That is the scene-graph **draw** broadcast (bank B, vtable slot +0x08), not the
update bank. The faulting object is therefore a node whose update method has
been running healthily all along and whose draw method is only now reached.
`jsrf_startup_probe` already dumps every live node at the update walker
(`pc=0x11083`); the cheapest next step is the same dump at the bank-B walker,
then match the node whose draw vtable holds `0x00177FE0`.

**Two measurements that redirect the first job.**

*There was no heap exhaustion in these runs.* `claude-apccheck-01/-02` contain
zero out-of-memory lines and exactly one indirect-call anomaly in the whole run
(`[ICALL] skipped not-code target 0x00000000`, once, immediately before the
fault). The title is rendering normally right up to it — `[GPU] draw #7600`,
`[FB] ... CHANGED`. So in at least this reproduction the fault is not a
failed-allocation consequence, and that branch of the investigation can be
dropped for it.

*Both live pointers at the fault are in the new MEM_RESERVE arena.*
`main.c:1102` calls `xbox_EnableSeparateReserveSpace(64 MB)`, and
`XBOX_TOTAL_RAM` is 64 MB, so the reserve arena is `0x04000000..0x08000000`.
At the fault `ECX = EBX = 0x042D0E60` and `EDI = 0x041E3010` — both inside it,
while the value AddRef actually dereferenced, `0xEF8F8F52`, is not a guest
address at all.

Two further facts constrain the cause:

- `sub_00177FE0` is **stdcall**: it reads `this` from `esp+4` and ends `ret 4`.
  The scene-graph walkers are **thiscall** — `sub_00011D00` sets `ecx = node`
  and pushes nothing. A node whose slot +0x08 lands on this AddRef would
  therefore read its argument off the stack and get whatever is there, which is
  exactly the shape of the observed registers: a sane `ecx`, a garbage `eax`.
- `0x00177FE0` is not a scene-graph draw method. It appears 32 times in
  `.rdata`, always at vtable+8 of a different class hierarchy (methods in
  `0x0015F000..0x0017C000`), i.e. it is a shared AddRef whose slot number
  happens to collide with the draw slot. No live node sampled before the fix
  has `0x00177FE0` in its draw slot — checked against every vtable in
  `claude-tickrate-noexec`.

**Hypothesis, not yet measured:** an object of that other hierarchy, allocated
in or reached through the reserve arena, is linked into the scene graph and
reached by the draw broadcast, which then invokes a stdcall AddRef through a
thiscall call site. The alternative is that the node is legitimate and its
vtable pointer is damaged. Both are distinguished by one observation: dump the
node and its vtable at the bank-B walker's call site, the way
`jsrf_startup_probe` already does at `pc=0x11083` for the update walker.

Note also that `RECOMP_PEEK` cannot see the reserve arena at all — both peek
implementations bound at `XBOX_TOTAL_RAM` — so objects above `0x04000000` must
be observed with a probe, not a peek.

**Acceptance:** no invalid dynamic indirect target or bad-pointer AddRef after
the title BGM begins decoding, followed by a measured successor to the title
screen's state 0x14.

### G15 — CLOSED, 5 September. It waits for nothing; the title screen finishes

`jsrf_title_state_probe` traces the 21-entry jump table at 0x001FA008 that
`sub_0004EF90` indexes by the object's +0x44. Global object id 8 walks
**0x12 -> 0x13 -> 0x14 in three consecutive visits**, no repeat between them,
and is never visited again. State 20 is the terminal teardown -- wait for
global object id 2 to be destroyed, unlink, destroy children -- so not being
visited again is what completing it looks like.

Sampling +0x44 would still have answered "state 20", the same as during the
stall. Only a trace separates arriving from sticking, which is why the probe
prints every change plus a heartbeat every 20000 unchanged visits.

So G5's premise is now disproven twice over: the title was never waiting for a
button, and it is no longer waiting at all. **Host input is still work worth
doing, but it is not a blocker and nothing is gated behind it.** Re-scope G5
from the first screen that actually reads a pad, not from this one.

### G22 — What is on screen after the title screen tears down

The live registered-object count reaches 0x80 and the update walker keeps
visiting nodes, so a successor scene exists and is being serviced. Nothing is
visible: `[FB] nonzero=1/153600 same`, and 11,498 draws arrive with input
coordinates of x -4636..4636, y -801..4172 -- world space, not clip space.

Two things to separate before anything else: whether the vertex transform is
being applied at all (the `xbox_vsh` path has its own tests), and whether this
content reaches a draw path the renderer implements, since the startup screens
did render. This is the frontier and it belongs with
`JSRF_GOALS_2026-09-03_RENDERER.md`.

**Measured, 5 September. It is not a transform problem, and not a draw-path
problem.** The vertex shader runs (executed batches=9915, rejected=0), 34,504
triangles rasterise, 5 batches are skipped as not screen-space and **zero** are
off-surface. The "input x -4636..4636" range is explicitly the *pre-shader*
range -- `draw_primitive` says so in its own comment -- so it describes object
space and carries no information about the transform. Do not cite it again.

What happens instead is that everything stops at once. Draws, triangles,
clears, VSH batches, the pusher's dwords and methods, and even its unhandled
count all freeze on the same report and never move, in every run.

*One defect found and fixed there* (commit a54ff90): on a backwards PUT with no
jump at the cursor, the feed parsed the ring's stale tail, desynchronised onto
last lap's ARRAY_ELEMENT16 index pairs, called them invalid headers and set the
permanent `stream_fault`. Invalid headers per run: 1+ before, 0 after.

*The remaining blocker is a rate, not a deadlock.* `sample` on the current
build puts **2485 of 2485** main-thread samples in one place: the D3D
pushbuffer free-space spin at `loc_001914F0` inside `sub_00191440`,

    ecx = [edx]        ; GET
    esi = edi - ecx    ; outstanding = PUT - GET
    cmp eax, esi
    jb  loc_001914F0   ; spin while needed < outstanding

`RECOMP_PB_WAIT_TRACE=1` shows it is not stuck -- GET does advance, and
`outstanding` sits at 6 against a `needed` of 2 -- but over 100 seconds it
advances about **four times a second**, across 40 million spin iterations. GET
is advanced by our own `jsrf_pushbuffer_ack` thread, whose loop is a
`Sleep(0)`, so four grants a second is the number that needs explaining, and
it is why a frame never completes and the framebuffer stays black.

The runtime's own threads are the suspects. In the same sample
`nv2a_ack_thread` spends ~37% of its samples in `__mprotect` (re-arming the
MCPX write trap) and another ~37% in `cthread_yield`. `mprotect` takes the
process-wide VM lock, and the main thread is spinning flat out on another
core. That is G16's subject arriving from a different direction.

**Acceptance:** the pushbuffer grant rate rises to something frame-shaped
(hundreds per second, not four), and a non-blank framebuffer follows.

### G5 — Host input, no longer a blocker (was: re-test after G14)

G5 is not disproven as *work* — the input backend is still needed — but its
premise as **the blocker** is. The title is waiting on the BGM, not a button.
Re-measure what the title screen waits for once G14 lands, and re-scope G5 from
the result rather than from the earlier reading.

**Acceptance:** a measured statement of what state 0x14's successor waits on,
taken after the ADX stream reaches PLAYING.

### G16 — Guest thread priorities are not honoured

`sub_0013B180` is CRI's idle/CPU-load measurement thread: `while (!exit)
counter++`. By design it runs at the lowest priority and soaks up slack. Here it
burns a full core, because our scheduler does not honour guest thread
priorities. That is very likely a contributor to the 233× rasteriser figure and
to the audio server running at half rate.

**Acceptance:** the idle thread yields to higher-priority guest threads; the
main thread's tick rate improves measurably with the idle thread present.

### G17 — The ADX server thread is vblank-paced at ~30 Hz, not 60

`sub_0013B1C0` spends 10,831 of 10,855 samples inside `sub_0018CE50`
(`D3DDevice_BlockUntilVerticalBlank`) and ticks ~30 times a second. Hardware
gives 60. Audio will stream at half rate even once G14 is fixed.

**Acceptance:** the server tick counter `0x0025EFAC` advances at ~60/s.

### G18 — Close the `0x0013F900` undetected entry point

The dispatch table skips from `0x0013F8F0` to `0x0013F910`, and the
disassembler recorded `XREF: 0x0013F900 (jump), 0x0013FE1B (data_imm)` — the
same shape as the `0x0013D840` hole recovered on 3 September. It is harmless
(the target is a deliberate halt stub) but it is a real gap, and
`recover_icall_entries.py` already does exactly this job.

**Acceptance:** `0x0013F900` appears in `recomp_dispatch.c`; ctest still 14/14.

## Readiness

Not playable, and not close, but no longer crashing. The audio stall and the
post-BGM memory corruption are both fixed, and the title now runs for as long
as it is given, past the state it was stuck in, with 127 live objects and a
live update walker. The title screen's own state machine now runs to
completion and tears itself down (G15). What it does not do is put anything on
screen: the successor scene's draws arrive in world coordinates. The frontier
has moved from "it faults" to "it renders nothing" -- G22 -- with G21's
general lifter fix still owed. No dead `_flags` fallback is reached by any
code this title now executes.

## Non-goals for now

No Windows/D3D11 build. No full JSRF regeneration. No upstream merge. No Metal
or GPTK renderer. Playable gameplay is a later milestone with no date.

**"Sound stays bypassed" is no longer a non-goal.** The previous list carried it,
and it hid the blocker for three sessions: the title screen will not advance
until the ADX stream reaches PLAYING. Decoding audio to an audible output device
remains out of scope, but the CRI streaming path must run.
