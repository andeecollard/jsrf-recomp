# Nearly every voice is replaying a stale buffer, and the instrument said so 457 times

19 September 2026. No run — read out of
`last-run-2026-09-19_BLACKSCREEN-FULL-GUEST-STOP.log`, which had
`RECOMP_VOICE_FRESH` armed.

## The reading

Final report, per voice, from `[VOICE-FRESH]`:

| voice | fresh | stale | stale % | longest stale run |
|---|---|---|---|---|
| 1 | 3 | 511 | 99.4% | 170 ms |
| 2 | 22 | 4,806 | 99.5% | 252 ms |
| **3** | 116 | 118,973 | **99.9%** | **65,933 ms** |
| 4 | 2 | 747 | 99.7% | 260 ms |
| 5 | 4 | 960 | 99.6% | 213 ms |
| 7 | 13 | 2,627 | 99.5% | 196 ms |
| 8 | 37 | 15,242 | 99.8% | 583 ms |
| 9 | 1 | 547 | 99.8% | 365 ms |
| 11 | 2 | 271 | 99.3% | 89 ms |
| 69 | 2 | 574 | 99.7% | 381 ms |
| **68** | **44,393** | **2,001** | **4.3%** | 66 ms |

**Voice 3 replayed one 32-sample slot for 65.9 seconds.**

## Voice 68 is the positive control, and it is what makes this readable

Every absence measurement here needs one, and this time it is in the data
rather than owed. Voice 68 is 4.3% stale in the same run, on the same
instrument, at the same moment. So the buffer-content hash does change when a
buffer is refilled, the classifier works, and 99.9% on the rest is a
measurement of the title and not of a dead counter.

## The instrument already said it, 457 times

The per-voice line carries its own verdict — `<- the guest is not refilling
this buffer` — and it appears **457 times** in that log. It is one line per
voice per report, buried among eleven others, with no total anywhere, so it
scrolled past.

There is a census line now: one line over all 256 voices, with the count at
≥90% stale, the worst replay in ms, and the freshest voice named beside it so
the control travels with the finding. It also says when there is NO healthy
voice, because then a stalled guest and a dead instrument look identical
again.

## What this is NOT established to mean

**"The guest is not refilling" is the instrument's wording, and it is one of
two readings.** The classifier hashes the buffer contents we read. Unchanged
contents mean either the guest never wrote, or *the guest's writes are not
reaching the memory we read*. This tree has already paid for that distinction
once: CLAUDE.md's guarded-MMIO note records 13.5M lost write windows that made
the guest look silent when it was not.

Deciding it needs a write-side observation, not a read-side one.
`RECOMP_APU_WRITE_TRACE` is armed in `paths.conf` and is the obvious place to
start.

## How this connects

- **G1.** The top open goal is why audio stops while the guest looks healthy.
  "Nine of eleven voices replaying stale buffers, one voice healthy" is a much
  sharper statement of the same failure, with a control attached.
- **The ADPCM over-read** ([the 11-block note](PROGRESS_2026-09-19_THE_ADPCM_FAILURES_START_EXACTLY_11_BLOCKS_FROM_THE_END.md)).
  A buffer that is never refilled has a tail that was never written, which is
  consistent with reading filler there. It does not explain why the boundary
  is a *constant* 11 blocks, so the two are related but not the same fact.

## A note on how this was nearly missed twice

I first grepped the first two and last two `[VOICE-FRESH-WIN]` lines, got all
zeros, and was about to write up "armed and measuring nothing". Those lines
are per voice: the first belong to idle voices 64–67 and the last to a
low-energy one. The same trap — reading one voice's row as if it were the
report — also produced a false "the ADPCM summary counter is dead" earlier the
same day, from comparing an early summary with a late detail line.

The rule in `MEMORY.md` is "read completed runs only". The sharper version
this pair argues for: **in a per-voice report, `head` and `tail` select a
voice, not a time.**
