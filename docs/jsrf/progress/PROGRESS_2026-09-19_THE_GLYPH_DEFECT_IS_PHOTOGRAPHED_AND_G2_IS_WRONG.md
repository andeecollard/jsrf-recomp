# The glyph defect is photographed, and G2's framing is wrong

19 September 2026. No run, no build — item 1 of
`HANDOVER_2026-09-19_THREE_FAILURES_ONE_BINARY_AND_A_TRAP_THAT_FINALLY_FIRED.txt`,
done against the captures already on disk.

Evidence preserved in `~/Library/Application Support/JSRF/glyphdump-2026-09-19-KEEP/`
with a README. **The originals would have been clobbered by the next run** —
the dump sequence restarts at 000.

---

## 1. The handover's count is wrong: 22 stills, not 99

`glyphdump/` holds **100 `report*.bmp`** (full 640×480 frames) and
**22 `still*.bmp`** (the 80×35 watched rectangle). The "99 rectangles on disk"
was the report dumps — `report000`–`report099` — not the still dumps.

Only **11 of the 22 stills are distinct**.

## 2. The still trap fired in two bursts and caught nothing

| dumps | mtime |
|---|---|
| still000–019 | **all 04:12:46** — one second |
| still020–021 | 04:13:14 |
| then nothing | for the remaining 7 minutes |

Content: a flat cyan/teal block with a sliver of glyph tops (000–019) and plain
scenery (020–021). **No text.** The region changes that fired were 7–49 pixels
of 2800.

The first dialogue box of the run appears in `report011` at **04:13:21** — after
the last still dump. The trap never looked at a dialogue box at all.

## 3. Why: the rectangle is in the wrong place

`RECOMP_FB_WATCH=280,410,80,35` → x=280..359, **y=410..444**.

Measured from the frames that contain a dialogue box, the text band is
**x≈150..620, y≈348..396** (line 1 glyph rows y=351..370; a second line runs to
y≈396). The rectangle sits **entirely below the text**, on the ground and the
minimap. `m026.png` shows it outlined in red against a live speech box.

Raising `STILL_PPM` from 1000 to 10000 was never going to help. The 47-pixel
story in the handover is about a rectangle that is not on the text.

## 4. THE DEFECT IS PHOTOGRAPHED — in the report dumps, not the still dumps

Three instances across the 7 dialogue lines the run rendered (`defects.png`):

| frame | time | rendered | should read |
|---|---|---|---|
| `report011` | 04:13:21 | `OK.  No▯ jump 3` | `Now` — the **w** |
| `report014` | 04:13:36 | `The ne▯t technique is a Grind.` | `next` — the **x** |
| `report026` | 04:14:26 | `You got some spra▯ cans, huh?` | `spray` — the **y** |

## 5. It is NOT a fixed character→glyph mapping — this refutes G2's framing

The same characters render **perfectly** in other boxes in the same run
(`clean.png`):

- **y** clean in `report013` "cock**y**", `report018` "Tr**y**", and
  `report028` five times — including the word "spra**y**,", *the same word that
  is corrupt in `report026`*.
- **w** clean in `report029` "**w**hen".

A static atlas-order index error would corrupt **every** occurrence. It does
not. Whatever selects the wrong glyph varies **per draw**, not per character.

## 6. What the replacements actually are: CJK glyphs, not ASCII punctuation

Thresholded pixel maps of all three, against their neighbours:

- bottom-aligned on the **same baseline** (y=370) as the glyphs either side;
- **taller than the Latin cell** — 20–23 rows against 15 for `a`, `c`, `e`;
- **normal cell width** (~11 px), and the pen advance is correct;
- `report014`'s replacement is recognisably **十** — a vertical stem with a wide
  crossbar. The other two are dense multi-stroke marks of the same kind.

So the 18 Sep readings "y → $" and "w → W" were **misreads of small kanji**, and
the inference built on them — *"the substitutions are NOT ASCII-adjacent, so any
index error is in FONT-ATLAS order, not character order"* — rests on that
misread. Do not carry it forward.

The signature — right pen position, right advance, **wrong cell height**, CJK
glyph — says the character was resolved through the **wrong sub-font / glyph
table**, taking that table's metrics with it. It does not say an index was off
by a constant.

**Not established:** what makes a given draw take the wrong table. Three
corruptions in seven lines is not enough to name the trigger, and the obvious
candidates (position in string, last glyph of a colour run, end of word, screen
x) each fit two of the three and fail on the third.

## 7. G2 does not need the still trap

The defect was caught by `dump_surface_bmp("report", seq++)` — one frame per GPU
report, unconditional, gated only on `RECOMP_FB_DUMP`. At
`RECOMP_REPORT_MS=5000` that is one frame every 5 s, and it caught **3
corruptions in 7 dialogue lines over 2.5 minutes**.

The defect is common. It needs no clever trigger — it needs frames with dialogue
in them.

## 8. The black screen, confirmed independently and dated

`report031`–`report099` are **69 consecutive byte-identical, entirely black
frames**, 04:15:01 to 04:20:41 — 5 min 40 s. `report030` (04:14:56) is the last
live frame and shows healthy gameplay with correctly-rendered tutorial text.

This corroborates section 2 of the handover from the **live surface**,
independently of the snapshot-based `nonzero=0/153600` reading.

## 9. Open, not claimed: the stills and the live frames disagree

At 04:12:46 the still dumps are a flat cyan block while `report004`, the same
second, is the title screen — no cyan anywhere in it. The two dumpers read
different buffers **by design** (`still` = `s_snap`, the presented copy;
`report` = the live surface), and `dump_snapshot_bmp`'s own comment says they
legitimately differ.

But every one of the 15 colours in those stills has **R≈0**, and every clear
colour logged in the run is also R=0 (`0x00000000`, `0x0000FFFF` cyan,
`0x000020E4`). That is consistent with the still trap comparing a **surface
freshly cleared to cyan** rather than a finished frame — which would make its
"region changed while the scene held still" verdicts meaningless.

**This is a hypothesis, not a finding.** It needs a run to settle, and it is
cheap to settle: dump `s_snap` whole next to the region.

---

## What the next session should change, in order

1. **Move the rectangle onto the text.** `RECOMP_FB_WATCH=150,346,360,28`
   covers line 1 of the dialogue box. paths.conf edit, no build.
2. **Keep `RECOMP_FB_DUMP` on regardless** — the periodic report dump is what
   actually works, and it needs `glyphdump/` emptied or re-pointed first, or
   the preserved capture is overwritten.
3. **Play the dialogue-heavy tutorial and collect boxes.** The defect rate is
   ~40% of boxes; twenty boxes is a corpus that can name the trigger.
4. Settle section 9 with a whole-`s_snap` dump beside the region.

---

# Addendum, same day: what the three corrupt glyphs are, and the instrument
# that was already watching

## The three replacements are three DIFFERENT glyphs

Thresholded bitmaps of all three, from the preserved capture:

| frame | replaced | rows occupied | baseline | shape |
|---|---|---|---|---|
| `report011` | `w` | 348–370 (**23**) | 370 | dense, multi-stroke; a smaller mark stacked above a larger one |
| `report014` | `x` | 351–370 (**20**) | 370 | a clean vertical stem with a wide crossbar — 十 |
| `report026` | `y` | 350–370 (**21**) | 370 | dense, multi-stroke, horizontal bars |

Neighbouring Latin glyphs (`a`, `c`, `e`) occupy **15** rows, 356–370.

So: **not one fixed fallback glyph.** Three different ones, each bottom-aligned
on the correct baseline, each in a correct-width cell with the correct advance,
each 5–8 rows TALLER than the Latin cell. `report011`'s appears to contain
fragments of two stacked atlas rows.

Together with the earlier finding that the same characters render correctly in
other boxes, that rules out both a fixed wrong index and a fixed wrong glyph.
The wrong sample location **varies per draw**.

## RECOMP_FF_BATCH_WATCH_TEX was armed the whole time, and unread

It has been in `paths.conf` since 17 Sep at `01737000`, and it produced
**14,273 lines in the very run that photographed the defect**. Nobody read
them. That is the third instrument in this defect's history to be built, armed
and never read.

Reading them found the instrument was broken rather than the title: it keyed
on the **vertex count alone**, so every six-vertex glyph quad collided into one
slot and every comparison between two different quads counted as a change. Its
documented discriminator — change means the data was wrong *before* the shader,
i.e. a race with the guest — would have been read off those 14,269 changes and
would have been wrong.

Fixed in `b362f37`: the key now carries the draw's ordinal within the frame.
Same texture and scene, the count goes from 14,273 to 145.

**So the discriminator is still unanswered, and is now answerable.** Arm
`RECOMP_FF_BATCH_WATCH_TEX` on the font atlas, reach a dialogue box, and read
the summary line:

- batches changing → the vertex data is wrong before the shader;
- nothing changing while a box renders corrupt → the defect is after the
  fetch: packing, the ring, or the vertex function.

**First, get the right texture offset.** `01737000` was never drawn in four
boot runs here. Offsets ARE stable across runs (75 of 75 identical between two
runs), so harvest the atlas's offset with `RECOMP_FF_BATCH_DUMP` in a run that
reaches dialogue and read `tex0=` off a six-vertex text batch — do not reuse
the number in `paths.conf`, which no run has confirmed.

Watch `slots`/`dropped` in the new summary: a font atlas draws dozens of quads
a line, and a change count taken while batches went unwatched means nothing.
