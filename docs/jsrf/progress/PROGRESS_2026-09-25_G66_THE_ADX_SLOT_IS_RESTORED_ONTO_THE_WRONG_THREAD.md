# G66 — the ADX lock restores one thread's priority onto another (25 Sep 2026)

Analysis of the two intermittent cutscene hangs (4:96 e231, 8:13 e291) from logs and code only. Summary in the 25 Sep goals file; the agent's working notes follow verbatim.

```
G66 notes (code+log reading only). Tools: rle.py (run-length of KeSetBasePriority trace), *.prio, census.txt.
Threads (from PsCreateSystemThreadEx stack tops): tib 0x4000 main (host 1005, handle 0x540003ED);
tib 0x917000 = sub_0013B1C0 ADX vsync thread (host 1007, handle 0x540003EF; BlockUntilVerticalBlank each pass);
tib 0x928000 = sub_0013B230; tib 0x939000 = sub_0013B2A0; tib 0x906000 = sub_0013B180 idle spinner.
Spin: sub_001437B0 loop { if GetThreadPriority(-2)!=15 break; call [0x2615F0] (=sub_0013B0E0 unlock) } -> ord 246/124/250 at 0x147D12.
Signature of poison in all 4 poisoned runs (s4m96 s8m13 s6m30 s6m61): "... (A16 A1)xN  A16  M1" = V raises (count 0->1, slot:=1), main's unlock to 0 restores V's slot onto main.
Then V is stuck at 16 (every later V lock saves 15: A16 A16), later main gets 15 (M16 M16), then SetThreadPriority stops entirely (count driven negative by main's spin).
Census (173 logs): block_releases>0 <=> locks-matched deficit>=1, in every build that has the counter; deficit occurs with real audio (rokkaku player logs, audio device 2) too.

```

## Findings (from the analysis report)

**The spin (confirmed).** Main thread (tib 0x4000) runs `sub_001437B0`
(gen recomp_0005.c:17059): `while (GetThreadPriority(-2) == 15) call [0x2615F0]`
— CRI's "drop all my nesting", using priority 15 as "I still hold the ADX lock".
`[0x2615F0]` = unlock `sub_0013B0E0`. GetThreadPriority is XAPI `sub_00147D12`
(ord 246 ObReferenceObjectByHandle, 124 KeQueryBasePriorityThread, 250
ObfDereferenceObject); it returns 15 when base priority is 16. In s4m96, 3.15e9
of 3.15e9 kernel calls; 1.047e9 unmatched unlocks, the count at 0x25EFA0 driven
negative; KeSetBasePriority (ord 143) never called again.

**Why (confirmed from the `[SCHED] KeSetBasePriority` trace).** Healthy:
`A16 A1` / `M16 M0` pairs (A = ADX vsync thread `sub_0013B1C0`, tib 0x917000).
Poisoned (s4m96 l.71578, s8m13 l.84136, s6m30, s6m61):
`(A16 A1)×N A16 M1` — A locks at count 0 and saves its priority 1 in the one
global slot 0x27D0F8; main unlocks to 0 and gets A's 1 restored onto itself.
Afterwards A never drops (`A16 A16`), main later locks at count 0 with 15 in the
slot and ends at 16 (`M16 M16`), then spins. Main can only enter while A is in
the region because the guard releases a holder blocked in a kernel wait
(`adx_guard_block_begin`, main.c:5102, kernel_bridge.c:2000; added 23 Sep).
"Released while the holder blocked" is 0 until the exact window of the first
unmatched unlock, then climbs ~300 per 5 s (vblank rate); r4m96 (good) has 0.

**What opens the window: a lock never unlocked.** Across 173 logs, block
releases > 0 appear iff locks − matched unlocks ≥ 1. Leak-only runs end their
priority trace on a single raise. Some show 1 SKIPPED unlock (a real unlock
from a host thread with guard depth 0, dropped). ~25% of harness runs leak and
~10% poison; the leak also occurs with real audio (rokkaku player logs), so
the silent driver is not the cause. Cause of the leak: unknown — a SKIP-dropped
unlock, lock/unlock on different host threads, or the guest really holding it.

**Fix direction.** (1) Restore per raising thread, never onto the unlocking
thread; (2) an unmatched unlock never drives the count below 0 and lets a
thread that holds nothing drop from 15, so the spin ends; (3) then fix the leak
using a trace (`RECOMP_ADX_TRACE`) of the last lock/unlock/block events with
host tid, guest return address, count, depth and priorities. Decisive run:
10 × `gw_run.sh … --env RECOMP_CHAPTER_JUMP=4:96` with the trace (leak ≈100–115 s
in, ~25–30 % per run → ~96 % odds of at least one).
