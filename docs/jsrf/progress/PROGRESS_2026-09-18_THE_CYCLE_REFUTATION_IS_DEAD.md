# The cycle refutation is dead — 18 September 2026, early hours

From the player's own session log (`~/Library/Application Support/JSRF/
last-run.log`, launched 22:40:02, crashed 22:44:21) and the crash report beside
it. Three separate results, one of which retracts an entry in the goals file's
REFUTED table.

## 1. THE HEADLINE: the voice list DOES contain cycles

The REFUTED table has carried this since 17 Sep:

> **The v1/v3 idle-trap storm is a two-voice list cycle** — `[APU-CYCLE]
> walks_with_a_cycle=0`, `[APU-WALKCAP] hit=0`. The detector marks each visited
> voice in a per-walk bitmap and never saw a revisit, so no walk ever met a
> ring.

The player's session says the opposite, in the same instruments:

    [APU-CYCLE] relink=90 (of 212 top inserts) walks_with_a_cycle=88 broken=0
                last=v3/list1 (cycle_break OFF)
    [APU-WALKCAP] hit=87

**88 walks met a cycle. The walk cap was hit 87 times. The voice that closed
the last one is v3 on list 1 — the 3D list — which is one of the two voices the
storm is made of.** Thirty-two entries in the idle-trap ring carry the `[C]`
flag, all of them v3.

**How the zero was produced, demonstrated rather than guessed.** A 240 s
scripted run taken tonight on the current build reports:

    [APU-CYCLE] relink=0 (of 4 top inserts) walks_with_a_cycle=0 broken=0
    [APU-WALKCAP] hit=0
    PLAYED: not asked -- gate 1 failed. (off=0 is title audio.)

Four top inserts against the player's 212. The run never reached gameplay, so
it never reached the state where the rings form, and every counter that depends
on that state read zero. **That is the same shape as the run the refutation was
written from.** A zero from a run that did not reach the state is not an
absence measurement — it is a missing positive control, which this tree's own
rules already require and which was not applied here.

**`RECOMP_APU_CYCLE_BREAK` is OFF in the player's config** (`broken=0` confirms
it never fired), so nothing has yet been measured about what happens when the
ring is broken.

**This does not make the cycle the cause of the music death.** It restores it
to an open hypothesis that was closed on bad evidence, and it is now the only
open one with a mechanism, a switch, and a number that moves in the player's
session but not in a scripted one.

## 2. The XC_AUDIO flip has NOT been heard

The player reported "sound still messed up" after tonight's stereo change. They
were not running it:

    engine written into JSRF.app   22:40:24
    player's session launched      22:40:02

Twenty-two seconds earlier, and their log proves it:

    [EEPROM] index 0x009 queried (first time), len=4, answered 0x00010001

That is the old mono+AC3 word. The bundle they launched was the one packaged
from a stale object earlier in the evening. **XC_AUDIO=stereo remains
untested by ear.** Relaunching is the whole of the test.

## 3. A new crash signature, distinct from the known one

Three `jsrf-engine` crash reports on 17 Sep:

    09:38  SIGSEGV  guest 0xFFFFFFBE  sub_001A2E2E <- 001A200D <- 001A241F
                                      <- 001A24BE <- 001A25AA
    16:16  SIGSEGV  guest 0xFFFFFFBE  identical
    22:44  SIGSEGV  guest 0xFFFFFFFF  sub_00011EE0 <- 000123E0 <- 00013A80
                                      <- 00013F80 <- 0006F9E0

The first two are the known DirectSound ISR crash — `001A24BE` is the ISR and
`001A25AA` is the routine that reads FECTL/FEDECMETH/FEDECPARAM. The third is
**not** that crash. It faults on `ESI=FFFFFFFF` used as a pointer:

    GUEST REGISTERS: EAX=00000000 ECX=D95AD9BB EDX=048475F8 EBX=00000000
                     ESI=FFFFFFFF EDI=D95AD9BB EBP=0050FE78 ESP=0050FEDC
    HEAP DIAGNOSTIC: head=BE820381 tail=417D3FCF flags=BEA51F60

`ECX` and `EDI` hold the same non-pointer word, and the heap header fields are
not plausible pointers. It is the first session on roughly ten hours of commits
the player had not run before, so it is new to them; whether it is new to the
tree is not established.

**Two changes from tonight are exonerated for it by evidence, not by argument.**
The stereo flip was not in the build they ran (§2). And G10's thunk-table change
cannot reach the guest: generated code never references
`xbox_kernel_thunk_table` — checked against the gen tree, which is the check I
failed to make when I first claimed the table was unreachable, having grepped
only `src/`, `templates/` and `tools/`.

## 4. A small one: the `[C]` flag is not in its own legend

`apu_vp.c:1781` emits `'C'` for `IDLE_TRAP_WHY_CYCLE`; the legend printed at
`:1802` lists only `L`, `N`, `P` and `R`. A reader of the player's crash dump
sees `3D:v3[C]<-TVL` with nothing to say what `C` means — and `C` is the flag
that turned out to matter tonight.

## 5. Tested and dropped: the probes are not destabilising it

The player's `paths.conf` carries four instrumentation switches
(`RECOMP_APU_WRITE_TRACE`, `RECOMP_VOICE_LIFECYCLE`, `RECOMP_FB_WATCH`,
`RECOMP_FF_BATCH_WATCH_TEX`), and `play_scripted.sh`'s own header warns that
heavy probes destabilise this title. Measured before saying so: **0**
`watch*.bmp` files written in the session's window, 44 `[APU-WRITE]` lines, 772
lifecycle lines in a 43,000-line log, and `[FB]` sampling once per second
rather than per frame. Light. The theory does not hold and is recorded here so
nobody re-forms it.

## What to do next

1. **Relaunch.** The stereo build has never been heard.
2. **`export RECOMP_APU_CYCLE_BREAK=1`** in `paths.conf` and play. One line, no
   rebuild, and `broken=` in the report says whether it fired. This is the first
   time there has been a reason to turn it on.
3. **The delivery measurement G1 has been waiting on still has not been taken.**
   `[APU-IDLE-DELIVERY] found idle=0, delivered=0` in tonight's scripted run,
   because the run never reached gameplay. It needs a player session, like
   everything else here.
