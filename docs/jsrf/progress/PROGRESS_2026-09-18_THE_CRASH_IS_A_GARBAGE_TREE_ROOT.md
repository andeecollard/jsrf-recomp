# The .text crash is a garbage tree root — 18 September 2026

Track D, characterised. Read-only: the `.ips`, the shipped binaries, the gen
tree, `src/`, and completed session logs. Nothing was built or run.

## The one-line answer

It is **not an ESI bug and not a heap bug**. `CActMan::m_lpActExecRoot`
(`CActMan + 0x87DC`) held `0xD95AD9BB` when `CActMan::IdleSub` walked it on an
ordinary tick. `ESI = 0xFFFFFFFF` is the first word of the resulting garbage
walk, and it faulted only because it happened to land in the runtime's one
unmapped hole near the top of the guest window.

## The fault, pinned to an instruction

    sub_00011EE0 + 0x7CC:   ldr w8, [x0]        <- PC
    guest:  mov eax, [esi]  at guest VA 0x00011F22, ESI = 0xFFFFFFFF

**No call ran in that frame**, which is what makes the path provable rather
than guessed: `HOST LR = sub_00011EE0 + 0x1E8`, and `+0x1E4` is the TLV
descriptor thunk from the entry sequence. So `bl sub_00011B90` at `+0x7A8`
never executed, and neither did `recomp_gpu_own_reconcile`.

The only path consistent with that leaves `ESI = MEM32(edi + 0x28)` where
`edi = ecx = 0xD95AD9BB` — the `this` pointer, garbage **on entry**. Three
independent cross-checks from the reported registers agree:

- `XBOX_PTR` masks to `uint32`, so the two reads at `loc_00011F10` wrap to
  guest `0x27` and `0x2F` — page 0, ordinary zeroed RAM since `RECOMP_TRAP_NULL`
  is not set. Predicted `EAX=0, EBX=0`; reported `EAX=00000000 EBX=00000000`.
- `eax == 0` there is exactly the condition that skips the call — which `LR`
  already told us independently.
- `ecx` is never written on that path and `edi = ebp = ecx` at entry; reported
  `ECX = EDI = D95AD9BB`.

`this` comes straight from `MEM32(CActMan + 0x87DC)` with nothing in between
(`recomp_0000.c:4295`). `+0x87DC` is `m_lpActExecRoot`, already named in
`CLAUDE_PROGRESS_2026-09-10_SYMBOLS.md`. **So the −1 is second-order; the
primary defect is the root pointer.**

It has only four writers in the whole gen:

    0x00011059  sub_00011000   [app+0x87DC] = MEM32(ecx+0x30)   unlink/pop
    0x000120D6  sub_00012020   [app+0x87DC] = esi               link/push
    0x00012C2E  sub_00012C10   = 0                              init
    0x00012C65  sub_00012C10   = eax                            init

## The instrument that lied, and what it cost

`HEAP DIAGNOSTIC: head=… tail=… flags=…` is `MEM32(0x00F80180)`,
`MEM32(0x00F80184)`, `MEM32(0x00F80018)` — three **hardcoded** guest addresses
(`main.c:3765`). They are not a heap header: `xbox_HeapAlloc` keeps its block
table host-side and writes no in-band metadata into guest memory. They are not
even at the heap base — the run banner puts the arena at `0x00510000`, so
`0x00F80180` is ~10.9 MB into it. The addresses appear to have been written
against the stale `0x00F80000` figure still in `docs/technical/memory-layout.md`
and `src/kernel/README.md`.

**Decisive:** the same three words appear byte-for-byte in the unrelated 09:38
DSOUND crash — `head=BE820381 tail=417D3FCF flags=BEA51F60` — different
session, build and faulting site. Verified directly against
`last-run-2026-09-17_crash-after-newgame.log`.

It cost a wrong sentence in yesterday's progress note, now retracted. Eleventh
instrument retired for lying, and the pattern is always the same: the label was
read, the code behind it was not.

## Why "seen once" does not mean "rare"

The guest window is 4 GiB; the mapped set is 128 MB RAM plus 27 mirrors, the
contiguous, tiled, NV2A, MCPX and Flash apertures. Unmapped holes total
**≈295 MB of 4096 MB — about 7%**:

    0x84000000-0x87FFFFFF  0xE8000000-0xEFFFFFFF  0xF8000000-0xFCFFFFFF
    0xFE000000-0xFE7FFFFF  0xFF100000-0xFFFFFFFF  <- this crash

The `.ips`'s own `vmRegionInfo` confirms the last one exactly. And
`0xD95AD9BB` did **not** fault: it falls in mirror slot 27 and aliases RAM
`0x015AD9BB`, inside the heap arena.

So **~93% of wild guest pointers are silently readable and writable here.** A
corrupted pointer crashes roughly one time in fourteen, and a wild *store*
corrupts RAM instead of faulting. This crash is the rare visible instance of a
class, not necessarily a rare event.

## Ranked, with the measurement that settles each

**M1 — a wild store scribbled the field.** Ranked first because the value has
none of the shape the four writers produce, and §above shows this runtime
manufactures that shape. *Measure:* `RECOMP_SCENE_REPORT=1` first (it prints
`root=` and the live `scene_root`, and is the positive control), then
`RECOMP_MEM_WATCH=<root+0x87DC>:4`, which logs the guest PC of every translated
store to that word. If the last store is not one of the four PCs, confirmed —
with the culprit's address. **Two env vars, no rebuild.**

**M2 — `sub_00011000` unlinked through a freed node.** It writes
`MEM32(ecx+0x30)`; if `ecx` is freed, that is whatever the allocator left.
The only writer that can propagate arbitrary garbage in one step. *Same watch;
look for `0x00011059` storing a non-pointer.*

**M3 — a cross-thread race.** Thread 15 was inside translated code at the
instant of the fault and three more guest threads were blocked in bridges.
Guest registers are `_Thread_local` so they cannot be cross-clobbered, but
**guest memory is shared and there is no guest-side lock**. *Cheapest first
cut costs nothing: a static call-graph query over the gen asking whether
`sub_0013B180/1C0/230/2A0` can reach any of the four writers.*

**M4 — a lifted epilogue losing `ebx`/`esi`/`edi`.** The tree has form: MSVC
switch tables parked mid-function have made the sweep lose `pop esi/pop edi`
before. *`-DJSRF_ABI_CHECK_ALL=ON` already exists and is OFF; needs a rebuild,
so not the first move. Positive control required: a run that never reaches
gameplay proves nothing.*

**M5 — a kernel/HLE bridge wrote through a guest pointer.** Last but not
dismissed, because `RECOMP_MEM_WATCH` **only sees translated stores** by
design. M1 coming back clean does not exonerate the runtime; it narrows to
this.

## Killed outright

- *"A callee clobbered ESI."* `LR = +0x1E8` proves no call ran in that frame.
- *"The guest heap is corrupt."* The words are identical in an unrelated crash.

## Two counters whose triggers were read before quoting

- `LAST INSTRUMENTED GUEST FUNCTION: sub_00177FE0` — set by one of only nine
  `RECOMP_TRACE_ENTER` probes in the gen, and `sub_00177FE0` is `AddRef`. It
  means "an AddRef happened at some point on this thread". **Not a locator.**
- `LAST 32 GUEST BLOCKS (0 available)` — this gen has no block probes. The
  zero means the instrument is absent, not that no blocks ran.

## Honest limits

Seen once. This characterises the instance precisely and establishes no rate,
trigger or repro. The 22:40 session log was overwritten, so the runtime's own
richer report is gone; only the `.ips` survives. And what `0xD95AD9BB` *is*
was not established.
