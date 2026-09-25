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
