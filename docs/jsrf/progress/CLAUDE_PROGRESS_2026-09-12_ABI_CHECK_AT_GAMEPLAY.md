# 129 functions return with callee-saved registers changed, and none of them are the jump

Date: 2026-09-12 (Europe/London)

`RECOMP_ABI_CHECK` is upstream's instrument for one specific failure: a callee
that returns with `ebx`/`esi`/`edi` altered, after which the caller "carries on
with a wrong loop cursor and simply does less work, with no error anywhere."
Half-Life 2 hit it when MSVC switch tables parked mid-function made the sweep
lose an epilogue, so `pop esi / pop edi` was never lifted.

It has been in this tree the whole time. `JSRF_STARTUP_ABI_CHECK` defaults OFF
and, when on, applies `RECOMP_ABI_CHECK` to `recomp_0000.c` **only** -- the
03 Sep handover says so in as many words. So the only code ever checked was the
code that runs before a frame. It was never pointed at gameplay.

## What it found

`JSRF_ABI_CHECK_ALL=ON` instruments all eleven generated chunks. One run to
gameplay under `RECOMP_FAKE_PAD=1` (19.3M triangles, so well past the intro):

```
129 distinct offending callee VAs
```

The logger deduplicates by callee VA, so that is 129 distinct *functions*, not
events. The original cap of 32 filled entirely during asset loading, which is
why raising it to 512 was necessary before anything at gameplay time was
reachable at all.

Cross-referenced against the sibling decompilation's 1,721 named functions,
bounded to offsets under 1 KB so a hit in an unnamed gap is not attributed to
whatever symbol happens to precede it:

| enclosing function | sites | clobbers |
|---|---|---|
| `FileManager::readSprNorm` | 10 | ebx |
| `FileManager::readStageObj` | 8 | ebx |
| `FileManager::readCarObj` | 8 | ebx |
| `Opening::Exec0Default` | 7 | edi |
| `Opening::drawDefault` | 5 | ebx esi |
| `FileManager::readPeople` | 5 | edi |
| `FileManager::readStage_MAYBE1` | 4 | edi |
| `FileManager::readMarkPressOrTex` | 4 | edi |
| `FileManager::readMissionDat` | 2 | ebx |
| `FileManager::readEffect` | 1 | esi |
| `__SEH_prolog` | 2 | esp, ebx esi edi |
| `__chkstk` | 1 | esp |

57 of the 129 land inside a named function; the other 71 are in unnamed gaps
and are not attributed here.

`__SEH_prolog` and `__chkstk` are **expected**: both move `esp` by design --
one builds an exception frame, the other probes the stack -- so "epilogue never
ran" is the check misreading a helper that is not an ordinary function. Three
of the twelve rows are therefore noise, and saying so is what makes the other
nine worth acting on.

The shape inside a reader is regular: a violation every 50-200 bytes through
the function, every one clobbering the *same single register*. `readCarObj`,
`readSprNorm` and `readStageObj` all lose `ebx`; `readPeople`,
`readStage_MAYBE1` and `readMarkPressOrTex` all lose `edi`.

## Two things this is not

**Not mid-function entries.** The obvious explanation is that the disassembler
promoted internal blocks to function entries and split the register save from
its restore. Tested directly against `midfunction_entries.json`: 206 recorded
mid-function entries, 129 offenders, **zero overlap**. The hypothesis is dead
and should not be retried without new evidence.

**Not the tutorial jump.** A first pass at this attributed hits to
`CPlayer::damageFire`, `CActBase::callExec0Default` and
`CMission::RunCmdsNoBlocking`, which would have been a direct explanation for
the animation gate and the 17x CPlayer update asymmetry. It was wrong. Those
came from a nearest-preceding-symbol lookup with no bound, attributing
addresses in unnamed gaps to symbols eight to forty kilobytes earlier -- an
offset of `+41750` is not inside anything. Bounded to 1 KB, **no offender falls
in `CPlayer`, `CActBase` or `CMission` at all.** There is currently no evidence
connecting this defect to the jump.

## What to do with it

The defect is real, measured, and concentrated in the asset readers -- which do
load stage objects, people and sprites, so wrong register state there could
plausibly corrupt loaded data. That is a hypothesis, not a finding.

1. Disassemble one offender against the binary -- `readStageObj+55` is the
   cleanest, at a round offset in a heavily-hit function -- and find whether
   the lifted C is missing a `pop`, or the function's extent is wrong, or the
   caller's expectation is.
2. Name the 71 unattributed offenders. The decompilation covers 1,721 of the
   title's functions; the gaps are where its coverage stops, not where the code
   stops.
3. Only then ask whether any of it reaches the jump. Nothing here says it does.

## Reproducing

```sh
cmake -S diagnostics/jsrf_first_fault -B <build> -DJSRF_ABI_CHECK_ALL=ON
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_FAKE_PAD=1 <build>/jsrf_first_fault ...
```

`RECOMP_FAKE_PAD=1` sends START for the opening window and then A only, which
is the variant that exists because a synthetic START pausing the game poisoned
an earlier input investigation.
