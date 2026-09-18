# The DSOUND crash is eating half the measurements — 18 September 2026

Two problems this project has been tracking separately are one problem.

## The finding

Every crashed run in every A/B taken today has the **same** signature:

    deferdepth2/t1_deferdepth21   CRASH at sub_001A2E2E
    deferdepth2/t5_deferdepth20   CRASH at sub_001A2E2E
    deferdepth2/t6_deferdepth21   CRASH at sub_001A2E2E
    deferdepth/t2_deferdepth0     CRASH at sub_001A2E2E
    deferdepth/t3_deferdepth0     CRASH at sub_001A2E2E
    deferdepth/t4_deferdepth0     CRASH at sub_001A2E2E
    nodepth/t1_nodepth0           CRASH at sub_001A2E2E
    synchist_degrade              CRASH at sub_001A2E2E

Eight runs, four A/Bs plus a standalone, all at `sub_001A2E2E +0x670` faulting
on guest `0xFFFFFFBE`. That is the known DirectSound ISR crash — `001A2E2E`
sits inside the **DSOUND** section (`0x0019E340–0x001BA89C`), XDK library code.

**`RECOMP_APU_FEDEC_HOLD` is on by default in every one of those runs.** The
guard G1a shipped for this crash does not prevent it.

## Why it matters more than "the title sometimes crashes"

It has been costing roughly **half of every measurement**, and the cost is not
evenly spread — it lands on whichever arm it lands on:

- `nodepth_ab` scored **n=1 vs 3** and was VOIDed. Two control runs lost.
- `deferdepth_ab` scored **n=1 vs 3** and was VOIDed. Three control runs lost,
  all three to this crash.
- `deferdepth2_ab` needed **six trials per arm** to yield two and four.
- The 400 s `RECOMP_SYNC_HIST` run died at the title screen after 6.4 s,
  producing two frame windows instead of the ~80 it was launched for.

Every VOID verdict and every "take at least two usable runs per arm" warning
this project has printed today traces back here. A defect that destroys half
the runs is not only a player-facing bug; it is a **tax on every measurement
the project takes**, and it has been paid silently because the harness excludes
those runs by design and says so politely.

## What this changes

The crash moves from "Track D, worth an hour" to the thing standing between
this project and cheap measurement. Fixing it roughly doubles the value of
every run.

**It is also not the crash Track D characterised.** That one — `sub_00011EE0`,
ESI=0xFFFFFFFF, a garbage `CActMan::m_lpActExecRoot` — is in `.text`, the
game's own code, and has been seen once. This one is in DSOUND, is seen
constantly, and is a different fault address (`0xFFFFFFBE` vs `0xFFFFFFFF`).
Two crashes, and the common one is the one nobody had been counting.

## What is NOT established

- **Why it fires.** `FEDEC_HOLD` holds the decode pair and the title still
  crashes, so the decode-pair race is *a* cause and not *the* cause.
- **Whether it has got worse.** Nobody counted crashes per A/B before today,
  so "eight today" has no baseline to compare against. It may always have been
  this bad and simply never been tallied.
- **Whether `RECOMP_SYNC_HIST` is implicated.** The 400 s run had it armed and
  crashed — but it crashed with the *same* signature as seven runs that did
  not, so there is no reason to attribute it to the histogram. **G4's parked
  halt is a different failure mode and this run says nothing about it.**

## The measurement it did produce

The histogram ran long enough to print, and with `no_depth_sync` now default
on:

    [SYNC] frames=2652 mean=3.14 ms p50=3.0 p90=4.5 p95=5.0 p99=8.5 max=10.8

Against the `[SYNC] p50 = 9.0 ms` the goals record. **Not comparable** — that
figure is from gameplay and this is the title screen, and scene-matching is a
rule here for good reason. Recorded only so the next run knows the instrument
works and roughly what to expect.
