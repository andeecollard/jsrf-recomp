# G7 is dead, the ADPCM gap is 704 samples, and the crash is not the merge's

19 Sep 2026, from one player session on the post-merge bundle (07:37–07:38,
35 s, reaching the opening tutorial) plus two scripted runs.

The session is preserved as
`~/Library/Application Support/JSRF/last-run-2026-09-19_MERGE-PLAYER-CRASH-ON-SKATE.log`
with its fourteen framebuffer captures in `glyphdump-2026-09-19-0738-MERGE-CRASH-KEEP`.
It was reported twice, as "crashed when I started skating" and "crashed when I
started new game". There is only one session on disk and one macOS crash
report: those are two descriptions of the same moment, the tutorial where the
player first moves.

## G7 is written off

The scatter-gather page-table bound was raised to explain the ADPCM refusals.
Its own code comment set the condition for abandoning it: *"If sge_oob stays
at 0 across a run that still refuses 4% of its ADPCM blocks, G7 is not the
cause and should be written off."*

```
player run     [APU-SGE] 8,427,817 translations, 0 implausible   (3.5% of blocks refused)
scripted run   [APU-SGE] 4,127,707 translations, 0 implausible
```

`calls=` is the positive control and it is enormous in both. The condition is
met twice over. **G7 is closed. Do not re-open it without new evidence of a
different kind.**

## Every other audio explanation is excluded too, each with a live control

| instrument | reading | its positive control |
|---|---|---|
| `[APU-BIN]` | 2D lost=0, 3D lost=0 | heard=67,450 and 2,718 |
| `[APU-PACE]` | starved=0 empty=0 | min_queued 26.7 ms, out_hz 48,006 |
| `[APU-IEN]` | 0 suppressed | 42 raises allowed |
| `[APU-SGE]` | 0 implausible | 8.4M translations |
| `[APU-ADPCM]` | **fail=1,779, all silenced** | ok=48,555 |

## What is left, and the discriminator that refuted itself

```
lowest failing block   v0: 208/219 (gap 11)    v69: 275/286 (gap 11)
every failure          hdr 08080808
v0   bs=36  ba=194032  ebo=14015  ba_blocks=5389   (non-stream)
v69  bs=72  ba=75696   ebo=18303  ba_blocks=1051   (stereo stream)
```

The discriminator installed for exactly this question predicted one of two
readings, and **both are refuted**:

- "the base is 11 blocks too high" wanted `ba/block_size == 11`
- "the buffer is 11 blocks shorter than `ebo`" wanted `ba/block_size == 0-ish`

Measured: 5,389 and 1,051. Neither.

The surviving fact is sharper than the one it replaces. Both voices begin
failing exactly **704 samples** from the end — 11 x 64, where 64 is
`ADPCM_SAMPLES_PER_BLOCK`. That is constant in *samples* while the two voices
differ in block size (36 against 72 bytes) and in length (219 against 286
blocks), so it is not a byte offset, not a page boundary and not a fraction of
the buffer. `hdr 08080808` on every failure says the tail is a fill pattern:
bytes nothing has written. The model believes the buffer is longer than it is,
or is taking its length from the wrong field. `nblocks = ebo /
ADPCM_SAMPLES_PER_BLOCK + 1` is where to start, and `ebo`'s inclusivity is the
question the next run has to answer against the guest's own declaration rather
than against itself.

A caution for whoever validates a fix: a scripted run the same day read
`ok=0 fail=0`, which by the instrument's own positive control means no ADPCM
voice played at all and says nothing whatever about the decoder. Confirm `ok`
is nonzero before scoring `fail`.

## The crash is not the merge's doing

```
EXC_BAD_ACCESS  KERN_INVALID_ADDRESS at 0x00000070ff555559
sub_00011D00  x13 recursive
  <- sub_000123E0 <- sub_00013A80 <- sub_00013F80 <- sub_0006F9E0
  <- sub_00147FB4 <- kernel_thunk_dispatch <- sub_00147F53
ECX=FF555555 EDX=3F800000 EBX=00FFFFFF ESI=80000000 EDI=FF555555
644 bytes of the 2 MB guest stack used
```

`sub_00011D00` walks a scene graph: `[edi+0x28]` child, `[edi+0x30]` sibling,
recursing on the child. It faulted on `MEM32(edi+4)` with `edi=0xFF555555`,
and guest base + 0xFF555559 is the reported address exactly, so the faulting
read is identified rather than guessed. `ecx == edi` means this was the first
iteration of a recursive call: the caller handed it a corrupt child pointer.

The fault site is new — it appears in none of the eighteen preserved player
logs. The upstream v0.10.0 merge landed the same morning and changed 1,422 of
9,102 generated bodies, so it was the obvious suspect. It is cleared:
**every function on that stack is byte-identical across the merge** except the
outermost, `sub_00147F53`, which gained one line, `ebp = g_ebp` at a thread
entry where `g_ebp` is thread-local and zero.

For completeness, the whole merge delta was classified rather than sampled:
~1,018 bodies are the mechanical x87 condition-code refactor, 404 are that one
added `ebp = g_ebp` line, 18 are a `sar` carry-flag respelling that extracts
the same bit, and 4 changed bounds — and those 4 are *fixes*, removing
overlaps where a function ran past the next function's start and emitted a
duplicate copy of it.

A clue for whoever picks this up: 0xFF555555 is opaque mid-grey in ARGB8888
and sits beside EBX=0x00FFFFFF (white, zero alpha) and EDX=0x3F800000 (the
float 1.0). The walk looks to have followed a pointer into colour or material
data, which puts the corruption one level above the frame that died.

The scripted `new_game` pad does **not** reproduce it. It reproduces a
different and already-documented fault, the DirectSound ISR at
`sub_001A2E2E +0x6A4` — which is useful in its own right, because that one
needs no human.

## The glyph trap has been aimed at the ground

It fired seven times this session and caught road texture every time. Counting
near-white pixels per row across the seven saved frames:

| frame | bubble | frame | bubble |
|---|---|---|---|
| report0 | rows 217–264 | report4 | rows 309–422 |
| report1 | absent | report5 | absent |
| report2 | absent | report6 | rows 43–123 |
| report3 | full-width flash | | |

**The speech bubble floats with the speaker over the whole height of the
frame.** No fixed rectangle can sit on it, which is the real reason three
successive aimings have each measured nothing. The speaker name label does not
move (y 417–436 for "Corn", y 413–425 for a longer name) and is what the
photographed corruption drew its stray glyphs onto, so `paths.conf` now reads
`RECOMP_FB_WATCH=120,410,400,30`. That rests on two frames, not twelve; if the
next session reports `comparisons>0 still=0`, re-measure before re-aiming.
