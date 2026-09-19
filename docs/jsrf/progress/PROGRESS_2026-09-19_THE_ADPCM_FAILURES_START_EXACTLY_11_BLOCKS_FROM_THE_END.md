# The ADPCM failures start exactly 11 blocks from the end of every buffer

19 September 2026. No run — read out of
`last-run-2026-09-19_BLACKSCREEN-FULL-GUEST-STOP.log`, which had the ADPCM
counters on (they are unconditional).

## The defect is live and large

Final report of that run:

    [APU-ADPCM] ok=4183185 fail=193693 short=0 oversize=0 silenced=193693
                (adpcm_guard on)

**193,693 refused blocks of 4,376,878 — 4.4%.** With the guard on, every one
is replaced by silence rather than the uninitialised stack array that used to
reach the mixer at full scale. So the audible symptom today is dropouts, not
noise.

`apu_vp.c` calls this defect open and records two theories that already died
against it: the segment descriptor's samples-per-block, and a page-table
translation going wrong past the first page.

## The new fact: the gap is a constant, and it is 11

Combining `[APU-ADPCM-PAGE]`'s per-voice lowest failing block with the buffer
size from the `[APU-ADPCM-FAIL]` ring:

| voice | first failing block | blocks in buffer | gap |
|---|---|---|---|
| v0 | 208 | 219 | **11** |
| v1 | 126 | 137 | **11** |
| v2 | 182 | 193 | **11** |
| v3 | 190 | 201 | **11** |
| v5 | 159 | 170 | **11** |
| v6 | 181 | 192 | **11** |
| v7 | 144 | 155 | **11** |

Seven voices, buffers from 137 to 219 blocks, the same 11 every time.

## What that rules out

- **A stride error.** A wrong block stride displaces block *n* by *n* times
  the error, so the first failure would land at a constant FRACTION of the
  buffer, not a constant distance from its end. The fractions here run 0.876
  to 0.950 and are not constant; the difference is.
- **The page-translation theory**, again and by a second route: 11 is not the
  113 blocks that fit in a 4 KB page, and the failing pages are many and
  various (pg54, 56, 61, 63, 66, 81, 90, 104) with several distinct translated
  PRD bases.

## What it points at

A **fixed over-read**: the fetch runs off the end of the real data by a
constant amount, the same for every voice. That is consistent with everything
else on record — block 0 always decodes (the comment in `apu_vp.c` calls that
"the whole finding"), the early blocks decode because they land on real audio,
and only the tail reaches whatever lies past the buffer.

Supporting it: **all 464 recorded failures carry the same header,
`0x08080808`** — one value, across 14 voices and dozens of block indices. Real
ADPCM headers vary with the predictor. One repeated value is filler, not audio
read at the wrong offset within the stream.

## Not established

Which side of the subtraction is wrong: whether the base is 11 blocks too
high, or the buffer is 11 blocks shorter than `nblocks = ebo /
ADPCM_SAMPLES_PER_BLOCK + 1` claims. Both produce this signature and they have
different fixes. `ebo` is the obvious next thing to read, against the guest's
own idea of the buffer length.

## What changed in the tree

`[APU-ADPCM-PAGE]` printed the failing block and not the buffer it was in, so
the constant was invisible — deriving it needed two log lines and a script. It
now prints `v0:208/219(gap11)` per voice, and lists 24 voices rather than 8.

**Unverified at runtime.** A boot run decodes no ADPCM at all
(`ok=0 fail=0`, which is the documented positive control for that line), so
the new format could not be exercised unattended. The numbers above come from
the 19 Sep gameplay log, and the change is a stored field and a format string.
