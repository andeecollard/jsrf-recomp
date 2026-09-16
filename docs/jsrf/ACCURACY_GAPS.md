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

**CORRECTED 2026-09-16, AND THE CORRECTION IS THE POINT.** The budget here was
quoted as `vsh 7.52 | submit 8.41 | sync 0.98 | rest (guest CPU) 15.03` of
31.94 ms, concluding "the guest-CPU half is as large as all graphics work
combined, so this is not a pure renderer problem". That conclusion was wrong,
in the way this project keeps being wrong: `rest` is a RESIDUAL, not a
measurement -- its own comment in nv2a_pb_exec.c says so -- and the quote has
no `clear` term because clear was not a separate stage when the number was
taken. With clear split out the same instrument reads `clear 12.47 |
rest 2.87`, and 12.47 + 2.87 = 15.34 against the quoted 15.03.

The 15 ms was the clear. Guest CPU was never half the budget; it was under 3 ms
then and is 3.6 ms of 16.4 now. It was almost entirely a renderer problem, and
treating it as one took the frame from 31.75 ms to about 16.4 -- see
handovers/HANDOVER_2026-09-16_SIXTY_FPS_AND_THE_PICTURE_IS_RIGHT.txt.

Budget as it stands, 2026-09-16, gameplay windows weighted by flips:
`vsh 3.3 | submit 4.1 | sync 5.2-6.1 | clear 0.06 | rest 3.6` of 16.4-17.0 ms.
Two runs of the same configuration measured 61.1 and 58.9 fps, so this is AT 60
rather than past it, and `sync` -- of which roughly 3.6 ms is GPU drain the CPU
is waiting on -- is where the remaining headroom is.

*Settled 2026-09-14: it is NOT wait-poll latency.* Four runs, paired by `live=`:

| live | poll 1000us | poll 100us | delta |
|---|---|---|---|
| 11 | 88.5 | 92.7 | +4.7% |
| 60 | 20.7 | 20.8 | +0.5% |
| 61 | 24.9 | 25.4 | +2.0% |
| 133 | 12.8 | 11.7 | -9.1% |

Mean +2.4% with the scatter straddling zero — noise. Tenfold finer polling buys
nothing, so the guest's 15 ms is real work or a different kind of blocking.
(Unmatched, the same runs "showed" +60%; that was a scene difference, and it is
why this table is paired by `live=`.)

*Still open:* what the 15 ms actually is. A prior profile says the guest is "93%
blocked". The standard architectural levers are already spent and measured:
flat dispatch does not appear in the profile at all, the icall feedback loop
changed nothing, and `-O2` on the title matched `-O0` within noise. Upstream's
own analysis says guest-registers-in-memory is "the single biggest performance
factor, and there is no fix short of a native backend" — which it also says not
to build. So this needs a profiler pointed at it, not more architecture.

### 2. Audio: the engine is down about half the time during real play
Two independent defects, both only visible with a controller in hand:

* **Trap storm.** Every voice retirement raises an APU front-end trap that stops
  the sound engine; the voice-list walk re-traps the same dead voice every frame.
  Real play: 109 voices started, 103 retired, **22,371 traps**, 48% of APU frames
  skipped.

  *2026-09-15, and read the whole of this before quoting any number above.*
  48% is a run-cumulative figure and it is diluted by a healthy first two
  minutes; differenced over the last 30 s of `20260915-110630` the engine is at
  **8.2%** duty and stays there. `RECOMP_APU_SE_WHILE_TRAPPED` is now **default
  ON** and takes duty from 63.6% to 98.6% over two matched pairs, with
  `RECOMP_APU_TRAP_COALESCE` (also default on) giving back 92% of the guest
  interrupts that costs. **The engine's frames are restored; the storm
  underneath them is not fixed** — a scene-verified mission still reports
  `suppressed=95721`. It has also never been heard: the gate on both switches
  is a person listening to a sustained sound at gameplay, and counters cannot
  close it.

  The producer is a `VOICE_ON` for a voice that is **already its list's head**,
  which stores `link(v) = regs[top] = v` and leaves the walk on a one-entry
  cycle. `regs[top]` is not stale and no guest write is missing; see
  `progress/CLAUDE_PROGRESS_2026-09-15_THE_HEAD_WAS_NEVER_STALE.md`, which
  retracts the "failed removal handshake" framing. `RECOMP_APU_SELFLINK_END`
  terminates such a list, is **untested** — its A/B had one usable arm — and is
  the next measurement, not the next edit.
* **Ring replay.** ~23% of the music ring is the previous lap replayed, because
  CRI's sound server gets 33.4 of the 43.1 passes/s it needs.
* **ADDED 2026-09-16 — the output clock was 48003 Hz, and it is the drift.**
  An EP frame is 256 samples at 48 kHz: 5333 **and one third** microseconds.
  `EP_FRAME_US` was 5333 and `throttle()` added it bare, pacing the model at
  48003.0 Hz against a device consuming at 48000. Measured from an existing log
  with no new run: `throttle=50636` over `elapsed=270.0s`, and
  `subframes/throttle = 7.99984` so one throttle is one 256-sample frame —
  12,962,816 samples in 270.04 s = **48,003.4 Hz**, exactly the truncation.
  `queued=` then climbs monotonically within a run (6144 → 10240 bytes over
  200 s, ~6 ms of audio lag per minute), and `apu_sdl2.c` flushes the whole
  queue at `APU_MAX_QUEUE_BYTES` — the audible jump at the end of a long
  session. **Fixed** by carrying the remainder (three frames = 16000 µs
  exactly); pinned by `jsrf_apu_pace` with no tolerance, because a tolerance is
  what hides a 62 ppm error. `out_hz=` had been printing 48003–48004 in
  thousands of report lines the whole time.
* **ADDED 2026-09-16 — the one-entry cycle above is the narrow case.** VOICE_ON
  for a handle already *anywhere* in the list takes the same TOP-insert branch
  and forms a ring of length ≥ 2, which `RECOMP_APU_REON_HEAD_NOP` does not
  cover and nothing counted. Existing transcripts already show them
  (`2D:v68[]<-v69` then `2D:v69[]<-v68`). Instrumented as `[APU-CYCLE]` plus a
  `C` flag in the idle ring; `RECOMP_APU_CYCLE_BREAK` is the behaviour change
  and is **off** pending a run count. See
  `handovers/HANDOVER_2026-09-16_THE_INSTRUMENTS_WERE_LYING.txt` §4.

### 3. Stability
* ~13% of runs SIGSEGV. *2026-09-15: NOT the OHCI path.* All ten faults recorded
  across today's scripted runs are the **same instruction** at the **same
  operand** — `sub_001A2E2E +0x670`, faulting on guest `0xFFFFFFBE` — and
  `0x1A2E2E` is inside the **DSOUND** section, i.e. recompiled DirectSound.
  `sub_001A2E2E` is the function that reads the guest's software previous/next
  links and updates the hardware voice list, so the crash sits in the same
  correspondence as the trap storm. `0xFFFFFFBE` is sentinel-shaped, not
  NULL-shaped, and `0xFFFF` is that list's own terminator. See
  `progress/CLAUDE_PROGRESS_2026-09-15_THE_CRASH_IS_ONE_INSTRUCTION.md`. The
  rate is unchanged and still not re-measured; what changed is that it has one
  address instead of a subsystem.
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
vanish on a clang upgrade.

**DONE 2026-09-14.** `-fno-strict-aliasing` is now tree-wide (installed before
`add_subdirectory`, because the punning is not confined to generated code —
`kernel_bridge.c`, `xbox_memory_layout.c` and the two nv2a pushbuffer files all
reach guest memory through the same macros). `-fwrapv` is on the title only: it
is about reproducing x86's wrapping signed arithmetic, and the runtime holds the
vsh rasteriser and the RGB565/depth loops that frame time actually goes into.

`-w` is replaced by `-Wall` minus four generator-inherent classes, not dropped.
Dropping it outright gives **11,318 `-Wunused-label` and 742
`-Wunused-variable`** across the tree — the generator emits a label per basic
block and a local per register it touches — which is not a signal anyone reads.
With those plus `-Wno-unused-but-set-variable -Wno-parentheses-equality`, the
measured yield over the whole gen tree is exactly two classes and no noise:

| count | warning | what it is |
|---|---|---|
| 440 | `-Wuninitialized` | `PUSH32(esp, ebp)` reading an indeterminate `ebp` |
| 11 | `-Wmacro-redefined` | `RECOMP_TRACE_ENTER`, defined by both `guest_trace.h` and `recomp_types.h` |
| 6 | `-Wmacro-redefined` | `XBOX_FS_BASE` — and these two definitions genuinely differ |

*That table was wrong twice before it was right, in the same way both times.*
"Zero warnings from the generated title" was claimed after grepping the build
log for `gen/recomp_*.c`, which does not match a warning reported against
`gen/recomp_types.h`. Both macro collisions hid in exactly that blind spot.
Count over headers too.

`XBOX_FS_BASE` was the one worth finding. `xbox_memory_layout.h` makes it the
constant `0x00001000`; `recomp_types.h` makes it the per-thread variable
`g_fs_base`. Include order alone decides which a translation unit gets, and it
currently gives each the one it needs — the runtime gets the constant, for
primary-TIB setup and for `kernel_bridge.c`'s "page zero is unmapped" bound,
while generated code gets the per-thread base it must have for `fs:[0]` and
`fs:[4]`. Benign today, and benign by accident of an include list: let
`kernel_bridge.c` include `recomp_types.h` and its null-pointer check silently
becomes "below this thread's TIB". Now an explicit `#undef` with the reasoning
written down, which preserves current behaviour rather than changing it.

**The 440 are the vindication of this section, with a twist.** They are real —
at `-O2` an indeterminate read is poison the compiler may propagate, not merely
stack residue — but `translator.py` already emits `ebp = 0` to fix them, in
b1f0531. The gen tree in `build-macos` was generated at 16:59 on 13 Sep and that
commit landed at 18:44, so **the binary every measurement since has used carries
all 440 anyway.** A regeneration is what clears them; the flag is what stops the
next gap going unnoticed.

The `-Wmacro-redefined` pair is behaviourally identical (the `recomp_types.h`
copy wins and differs only by a `(uint32_t)` cast), so it is noise — but it is
the kind that hides a real collision later, and should be guarded with `#ifndef`.

Still not done: **one gameplay run under `-fsanitize=undefined,alignment`**,
which has never happened and would have caught the shift and float-cast bugs
directly.

### 5. `fistp` never got the float→int fix the SSE path got — under `_ftol2`
`lifter.py:3155` emits `(int32_t)llrint(...)`. x86 `fistp` gives the integer
indefinite `0x80000000` for NaN/inf/out-of-range; `llrint` on AArch64 saturates
and returns 0 for NaN. The helpers (`RECOMP_F2I_*`) already exist and this path
does not use them.

`sub_0017C3E8` is MSVC's `_ftol2` — *the* `(int)float` conversion for the whole
title.

**FIXED 2026-09-14**, and three of this section's numbers were wrong:

* **Callers: 432 call instructions across 129 distinct functions**, not 172.
  (The disasm header's own "Called by" list says 92; 172 matched nothing.)
* **There are only 6 `fist`/`fistp` sites in the entire image** — 1 `fist
  dword`, 4 `fistp dword`, and the 1 `fistp qword` inside `_ftol2`. The fix is
  correct but far smaller in blast radius than this section implied.
* **`_ftol2`'s int32 behaviour does not change.** It converts via *qword* and
  then corrects toward zero from the residue with ordinary integer arithmetic
  we already lift, so the fix alters only ±inf and |v| ≥ 2^63 there. NaN
  returns 0 either way, because the indefinite's low dword is 0 and the guest's
  own `test eax,eax` path returns eax. The 4 direct `fistp dword` sites get the
  real benefit.

`_ftol2` also does **not** reprogram the control word, so nearest-even was and
remains the right rounding mode; `llrint` already rounded that way. What was
wrong was only the NaN/inf/out-of-range answer. Measured on this host:
`(int32_t)llrint(NaN)` = 0, `(+inf)` = 0xFFFFFFFF, `(3e9)` = 0xB2D05E00, where
x86 stores 0x80000000 for all three. Now via `RECOMP_F2I16/32/64_ROUND`.

### 6. Missing kernel export that DSOUND uses for audio
**`KeSynchronizeExecution` (ordinal 153) is unbridged**, and an unbridged
ordinal returns `g_eax = 0`. 102–229 calls per session.

**CONFIRMED BY RUN 2026-09-14** — `[KERNEL] WARNING: no bridge for ordinal 153
(slot 117), returning 0` appears in a 250 s gameplay-schedule log, alongside the
same warning for 204. Both gaps are live, not static readings. **FIXED** the
same day.

One correction: `g_eax = 0` is STATUS_SUCCESS for the NTSTATUS export (204), but
`KeSynchronizeExecution` returns **BOOLEAN**, so the guest was handed a
fabricated **FALSE** — the routine's answer, invented, without the routine ever
running. For the call site that loops retesting a flag, that is a loop whose
exit condition nobody computes. Also: the stack was never at risk for either
ordinal, because both already had `stdcall_args_for_ordinal` entries, so the
damage was confined to the return value and the unwritten out-params. Both
guest call sites are DSOUND, on exactly the two interrupt objects JSRF connects
for audio (vectors 5 and 6), and one of them loops retesting a flag. The guest
believes its interrupt-synchronised audio work ran. Nothing ran.

Both routines already exist in the recompiled dispatch, so bridging it is small.

### 7. `NtProtectVirtualMemory` returns success and writes nothing
Ordinal 204, **4,089 calls** — the hottest unbridged export by 40×. One guest
call site: XAPI's `VirtualProtect`. Two lies: no protection changes, and
`lpflOldProtect` is never written, so the guest's wrapper returns uninitialised
stack as "previous protection".

**FIXED 2026-09-14 as a ledger, deliberately not as an `mprotect`.** There was
no per-page protection store to reuse — `xbox_MmSetAddressProtect` forwards to
host `VirtualProtect`, and `xbox_MmQueryAddressProtect` asks host `VirtualQuery`,
whose macOS shim (`win32_compat.c:1489`) **fabricates PAGE_READWRITE for every
address**. So ordinal 204 now keeps one 16-bit Xbox protection per 4 KB guest
page (64 KB of BSS over 128 MB, contiguous mirror folded onto the RAM it
aliases), does NT's page rounding on both in and out parameters, validates
`NewProtect`, and reports the first page's previous protection through
`OldProtect`. Ordinals 179 and 182 now read and write the same ledger.

It calls **no host `mprotect`**, per CLAUDE.md's rule: macOS host pages are
16 KB, the APU aperture is trapped read-only, and a guest protection change
sharing a host page with a trapped aperture opens exactly the window that
swallowed every `VOICE_ON`. A page the guest marks PAGE_READONLY therefore stays
writable; what it gets is a consistent story rather than uninitialised stack.

*Related, and measured rather than assumed:* `bridge_MmSetAddressProtect`
(ordinal 182) **does** reach host `mprotect` on guest memory, which is that same
hazard. It is **latent here, not live** — ordinal 182 does not appear among the
23 ordinals JSRF calls in a 250 s run. That is "not hot" rather than "never":
the kernel log budget caps logging, so a rare late call would not show. Worth
closing for other titles.

### 8. Signed conditions ignore the overflow flag
`lifter.py:594-747`. **FIXED 2026-09-14, and this section was half wrong.**

*Wrong about `sub`.* `sub` never reached the SF-only fallback: `COND_MAP`
supplies `CMP_L/CMP_GE/CMP_LE/CMP_G`, `sub` always has two operands, so the
`if cmp_macro and rhs:` reconstruction above them always won — and at 32 bits
that reconstruction **is** `SF != OF` and was already correct. The SF-only claim
holds for **`add` only**. `sub`'s signed conditions were wrong only at 8 and 16
bits.

*Right about width.* "Evaluated at 32 bits over zero-extended `LO8/LO16`, so
`sub al,bl; js` can never be true" is **verified true**: `LO8`/`LO16` and
`MEM8`/`MEM16` all zero-extend on promotion, the emitted condition was
`((int32_t)LO8(eax) < 0)` — a compare of 0..255 against 0 — and compiling and
running it for `al=0, bl=1` (x86 SF=1) returns 0.

Both are now fixed, tested by an exhaustive sweep of 6 conditions × 3 widths ×
2 mnemonics × 196 operand pairs against a reference computing ZF/SF/OF from
x86's definitions, with a negative control confirming the old expressions fail it.

**FINISHED 2026-09-14 (later), across all five remaining setters**, with three
defects the section did not mention:

* `and`/`or`/`xor` answered `jbe` as 0 and `ja` as 1. Only CF is cleared by
  those — ZF is not — so `jbe` is ZF and `ja` is `!ZF`. `and eax, eax; jbe` was
  never taken where x86 takes it.
* `neg` had no `jbe`/`ja` case and fell through to the always-false `_flags`
  fallback. After `neg`, `CF || ZF` is always 1, so `jbe` should always be
  taken and never was.
* `neg`'s CF cases were **not** affected by width — "is this nonzero" survives
  zero-extension — so they were left alone rather than changed to look tidy.

`sar` was the serious one, and a different class: a wrong **value**, not a wrong
flag. `(int32_t)LO8(eax) >> n` over a zero-extended byte is a logical shift in
an arithmetic cast. `sar al, 1` with `al=0x80` gave 0x40 where x86 gives 0xC0 —
a negative number quietly halved into a positive one.

**Bounded honestly: it changes nothing for JSRF.** The regenerated tree has 673
arithmetic-shift emissions and every one is 32-bit; there is no narrow `sar`
anywhere in this image. Same for the `js`/`jns` fix on `shr`, whose destination
can never have its MSB set for a nonzero count. These are correctness insurance
for the translator, not a behavioural change to this title, and no run is
claimed for them.

**Still open in the shifts:** 16-bit `shld`/`shrd` shift the incoming bits in
from the wrong end (`shld ax, bx, 4` with both 0xFFFF gives 0xFFF0 where x86
gives 0xFFFF), and a zero masked shift count is treated as setting flags where
x86 leaves them untouched.

*Superseded — the original note, kept because it is what the next section of
work was planned from:*
**The same defect survives elsewhere, found but out of scope that day:** `neg`
(`lifter.py:733-743`) has *both* halves — it sets OF=1 when the operand was the
width's most-negative value, so `jl` after `neg al` is wrong for `al=1`;
`adc`/`sbb`, `and`/`or`/`xor`, `shl/shr/sar` and `shld/shrd` each read
`(int32_t)` at 32 bits over possibly-narrow operands (`or al,al; js` is a common
MSVC idiom and can never be true today). `and`/`or`/`xor` are sound on OF, which
is architecturally 0 there. **This is the next lifter job.**

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

Items 1, 2 and 4 of the original list were done on 2026-09-14; what follows is
what is left, re-ranked.

1. **Regenerate, and keep regenerating.** The single highest-value thing found
   this session was not a bug but a *stale artifact*: the gen tree predated its
   own `ebp = 0` fix by two hours, so 440 uninitialised reads were compiled into
   every binary measured since. Translator fixes do nothing until the title is
   regenerated, and nothing in the build warns you.
2. **One gameplay run under `-fsanitize=undefined,alignment`.** Still never done.
   Now that a schedule can reach gameplay unattended, it is finally cheap.
3. **Hear `RECOMP_APU_SE_WHILE_TRAPPED=1`.** Built since edbd87d, still never
   evaluated. `play_scripted.sh` + `gameplay.pad` is the A/B harness for it;
   the remaining human step is listening.
4. **Finish the signed-condition fix** — `neg` especially, which has both the OF
   and the width defect. See §8.
5. **Settle what the 15 ms of guest CPU is.** Partly answered: a 10 s `sample`
   at gameplay is ~93% blocked (`__semwait_signal` 330, `__psynch_cvwait` 271,
   `semaphore_wait_trap` 209 …) against ~100 samples of real compute, of which
   the largest single entry is `sub_0013B180` — a guest spin on `0x0025EFC0`.
   That reproduces the earlier "93% blocked" profile *at gameplay* rather than
   at the title. The open part is why the guest is waiting, not whether it is.

## Method note

Two findings in this document exist only because a person played the game with a
controller. **Every scripted run in this repository parks the player** — no
skating, no grinding, no sound effect ever finishes, `on=5 off=0 idle_trap=0` —
so the trap storm and the hotplug abort were unreachable by the test harness for
the life of the project.

*2026-09-14:* `pad/gameplay.pad` and `play_scripted.sh` are the first attempt at
closing that. The schedule skates, jumps and sprays; the runner uses play.sh's
light environment rather than run_scripted.sh's four heavy probes, which
provoke the input-poll stall and make a gameplay measurement measure the probes.

**It is not yet proven to work, and the first version of it failed instructively.**
It ran the full 250 s, fired 186 of 193 events, retired one voice — and never
left the attract screen: 1342 `NtOpenFile` calls, the title plateau, against New
Game's 1408. `off=1` read as success until the open count was checked.

*2026-09-15:* **those two numbers were measuring the disc cache build, not the
screen.** They are left above because the conclusion drawn at the time — that
the run never left the attract screen — was independently right, but the gate
is retired: on a pre-cached emulated HDD the count is 131 in every scene. The
scene gate is now `[JSRF-SEQ] now=` under `RECOMP_SEQ_REPORT`. The
runner now tests *reaching* gameplay and *playing* as two separate gates, and
the schedule now uses new_game.pad's full measured boot prefix rather than
moving.pad's truncated one. The next run is what settles it.
