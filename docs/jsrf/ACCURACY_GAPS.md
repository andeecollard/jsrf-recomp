# JSRF recomp — accuracy and playability gaps

2026-09-14. A systematic audit across four subsystems, to answer one question:
**what should we fix to make the game more playable and more faithful?**

Every row is evidence-backed. Where a claim is static-only it says so, and names
the run that would confirm it. Ordered by impact, not by area.

---

## Tier 1 — playability. The game is not yet enjoyable because of these.

### 1. Speed: we run at a half to a fifth of the real frame rate
**The dominant issue. Everything below is secondary to it.**

| | scene | fps |
|---|---|---|
| xemu (reference) | in-engine attract | 59.2 – 60.1 |
| ours | same attract | 11.7 – 14.3 |
| ours | gameplay, Dogenzaka Hill | 31.3 |

JSRF is a 60 fps title (measured, `nv2a_pgraph_flip_stall`, same event our
`[FRAME]` counter uses). Era-typical fixed-step simulation means half the frame
rate is *half the game speed* — it looks slow, not choppy.

Gameplay frame budget: `vsh 7.52 | submit 8.41 | sync 0.98 | rest (guest CPU)
15.03` of 31.94 ms, against a 16.67 ms budget. **The guest-CPU half is as large
as all graphics work combined**, so this is not a pure renderer problem.

*Open question, unresolved:* is that 15 ms compute or blocked waiting? A prior
profile says the guest is "93% blocked", and `bridge_KeWaitForSingleObject` is a
1 ms poll rather than a wait. An A/B of poll granularity at gameplay was
attempted and **thrown away as invalid — the two runs reached different scenes**
(live=161 vs live=61). Redo it scene-matched before believing any result.

### 2. Audio: the engine is down about half the time during real play
Two independent defects, both only visible with a controller in hand:

* **Trap storm.** Every voice retirement raises an APU front-end trap that stops
  the sound engine; the voice-list walk re-traps the same dead voice every frame.
  Real play: 109 voices started, 103 retired, **22,371 traps**, 48% of APU frames
  skipped. A fix exists behind `RECOMP_APU_SE_WHILE_TRAPPED=1` — **built, off,
  and never heard**.
* **Ring replay.** ~23% of the music ring is the previous lap replayed, because
  CRI's sound server gets 33.4 of the 43.1 passes/s it needs.

### 3. Stability
* ~13% of runs SIGSEGV in the OHCI path (rate not re-measured since timing changed).
* **Controller hotplug aborts the process**: disconnect then reconnect →
  SIGABRT immediately after `SDL_GameControllerOpen` returns
  (`src/input/xinput_device.c:346`, reached from guest threads). No backtrace
  yet; lldb needs SIGSEGV/SIGBUS passed through or it breaks the MMIO traps.

---

## Tier 2 — accuracy. Cheap fixes that sit underneath everything.

### 4. The generated C is compiled with undefined behaviour left undefined
`-O2 -w`, with **no `-fwrapv` and no `-fno-strict-aliasing`** (verified: zero
hits across every CMakeLists/sh/py). 1,400 signed-overflow multiply sites from
2-operand `imul`, 484 `neg`, 339 div blocks; and `MEM8/16/32`, `MEMF`, `MEMD`
all reach the same guest bytes through different `volatile T*` — `volatile`
suppresses elision, not type-based aliasing. `-w` silences every warning on
40 MB of generated code.

This is the "plausible wrong numbers everywhere" class, and it can appear or
vanish on a clang upgrade. **Two flags.** Then one gameplay run under
`-fsanitize=undefined,alignment`, which has never been done and would have
caught the shift and float-cast bugs directly.

### 5. `fistp` never got the float→int fix the SSE path got — under `_ftol2`
`lifter.py:3155` emits `(int32_t)llrint(...)`. x86 `fistp` gives the integer
indefinite `0x80000000` for NaN/inf/out-of-range; `llrint` on AArch64 saturates
and returns 0 for NaN. The helpers (`RECOMP_F2I_*`) already exist and this path
does not use them.

`sub_0017C3E8` is MSVC's `_ftol2` with **172 callers** — it is *the* `(int)float`
conversion for the whole title.

### 6. Missing kernel export that DSOUND uses for audio
**`KeSynchronizeExecution` (ordinal 153) is unbridged**, and an unbridged
ordinal returns `g_eax = 0` — STATUS_SUCCESS. 102–229 calls per session. Both
guest call sites are DSOUND, on exactly the two interrupt objects JSRF connects
for audio (vectors 5 and 6), and one of them loops retesting a flag. The guest
believes its interrupt-synchronised audio work ran. Nothing ran.

Both routines already exist in the recompiled dispatch, so bridging it is small.

### 7. `NtProtectVirtualMemory` returns success and writes nothing
Ordinal 204, **4,089 calls** — the hottest unbridged export by 40×. One guest
call site: XAPI's `VirtualProtect`. Two lies: no protection changes, and
`lpflOldProtect` is never written, so the guest's wrapper returns uninitialised
stack as "previous protection".

### 8. Signed conditions ignore the overflow flag
`lifter.py:594-747`. After `sub`/`add`, `jl/jge/jle/jg` emit `result < 0` — SF
alone, where x86 is `SF != OF`. Wrong on any subtraction that overflows, at
every width. `cmp`/`test`/`inc`/`dec` were fixed; their siblings were not.
Also evaluated at 32 bits over zero-extended `LO8/LO16`, so `sub al,bl; js` can
never be true.

### 9. Smaller, confirmed
* `div`/`idiv` by zero is silent (127 + 114 sites): AArch64 returns 0 and
  carries on where x86 faults.
* `xbox_GetConnectedInterrupt()` indexes `g_interrupts[vector]` but
  `KeConnectInterrupt` fills that array **compactly**. It works today only by
  coincidence of JSRF's connect order.
* `MmGetPhysicalAddress` returns the VA unchanged; correct below 64 MB, wrong
  for contiguous allocations at `0x8xxxxxxx`. Works only because the APU masks.
* `KeSetTimer` never signals the KTIMER, so a wait on one can never be satisfied.
* `HalReadWritePCISpace` reads nothing, so a guest read-modify-write of PCI
  config ORs into uninitialised stack.
* `fld xword` reads 4 bytes as a float (3 sites, CRT only).

---

## Audited and found CLEAN — do not spend time here

* **The renderer's method coverage.** "392 distinct unhandled methods, 85% of
  all methods" is a **red herring**: `note_unhandled` means "no explicit case in
  the switch", not "dropped". Every method < 0x2000 lands in `s_methods[]`
  first, and the fixed-function path reads it back (`nv2a_ff.c:22` consumes
  `SET_COMPOSITE_MATRIX` directly). The real numbers: **VSH 314,598 batches with
  164 rejected (0.05%), textures 697,789 prepared with 0 rejected**, 700k draws,
  68.7M triangles.
* **Kernel file I/O, async APC completion, save/persistent storage, EEPROM and
  clocks, DPCs, events** — all genuinely implemented against real host calls.
* **Lifter**: x87 80-bit precision (double is the right model here — no
  `_controlfp(_PC_24)` anywhere), `minps`/`maxps` NaN handling, `shufps`,
  direction-flag and string ops, `movzx`/`movsx`, `lock` prefixes, AF and parity,
  jump tables, endianness.
* Previously-open lifter items `cvtss2si` rounding, NaN float→int casts and
  shift-count masking are **now fixed**; only the `fistp` path was missed.

---

## Suggested order

1. `-fwrapv -fno-strict-aliasing`, drop `-w`, then one UBSan gameplay run.
   Cheapest thing here and it sits under every other number.
2. Bridge `KeSynchronizeExecution`; write `lpflOldProtect`. Both small, both
   affect audio and memory correctness the guest currently cannot see.
3. Hear `RECOMP_APU_SE_WHILE_TRAPPED=1` with a controller and a grind.
4. Swap `fistp` onto the existing `RECOMP_F2I_*` helpers.
5. Redo the wait-poll frame-rate A/B **scene-matched**, and settle whether the
   15 ms of guest CPU is compute or blocked waiting. That answer decides whether
   the 2x speed gap is a renderer problem or a scheduling one.

## Method note

Two findings in this document exist only because a person played the game with a
controller. **Every scripted run in this repository parks the player** — no
skating, no grinding, no sound effect ever finishes, `on=5 off=0 idle_trap=0` —
so the trap storm and the hotplug abort were unreachable by the test harness for
the life of the project. A pad schedule that actually plays is worth more than
any probe here.
