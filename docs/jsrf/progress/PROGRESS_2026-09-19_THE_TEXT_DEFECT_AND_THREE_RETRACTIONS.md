# Starting on the text defect: what is established, and three things I retracted

19 September 2026, late afternoon. Follows
`PROGRESS_2026-09-19_THE_BLACK_SCREEN_IS_THE_THIRD_SWITCH_ARM.md`. No fix here.
This records what was measured, what was refuted, and why the cheap offline
route is the weaker one -- so the next person does not spend the afternoon I
spent.

## The symptom, from the player's captures

Two distinct symptoms, and they may or may not be one bug:

  - SUBSTITUTION. `y` renders as `S`, `0` as `D`, `2` as `Z`. NOT a fixed
    character->glyph mapping: `Try it again` renders the `y` correctly while
    `ot too shabbS, kid.` renders the same letter wrong, minutes apart in one
    session. This corroborates section 5 of
    `PROGRESS_2026-09-19_THE_GLYPH_DEFECT_IS_PHOTOGRAPHED_AND_G2_IS_WRONG.md`.
  - TRUNCATION, which is new. Leading characters are lost, and the count
    varies: `Collect` -> `llect` (2), `Not` -> `ot` (1),
    `Farside Stab Soul` -> `de Stab Soul` (5). One instance,
    `llect 10 SpraS Cans`, begins FLUSH against the left edge of the window,
    which is what a line centred on a mis-measured width looks like. If a
    wrong glyph carries a wrong advance, substitution and truncation are one
    bug. The `Farside` banner is NOT flush left, so that case is unexplained
    by this and may be a second mechanism.

## The font is shared across five languages and is Japanese-scale

The disc ships `List_{eng,frn,ger,jpn,spa}.dat` (~405 KB each) and
`tex_{eng,frn,ger,jpn,spa}.dat` (7.85 MB each) -- five languages -- against
ONE font: `Mark/font1.dat` (3.0 MB) plus `Font/jetfont.dat` (1.18 MB). 3 MB is
far beyond what Latin needs, so a Japanese-capable glyph set is in play and any
index error has a large space to land in. The photographed substitutions are
all Latin->Latin, so if there is an index error it is landing inside the Latin
region; A KANJI APPEARING MID-ENGLISH-WORD WOULD PIN IT as a large one, and is
worth watching for.

## RETRACTION 1: font1.bin is not a character->glyph table

I read its first records as character-range -> glyph-range mappings
(`0x20->0x30`, `0x40->0x50`, `0x60->0x70`). Parsing all 370 refutes it. The
header is `NORM` + count 370, then 370 x 32-byte records, and 11856 - 16 =
11840 = 370 x 32 exactly. Every record is byte-identical except two fields:

    f0 = 32*(i+1)      f1 = f0 + 16
    f2=0xC  f3=0  f4=2  f5=0x34D8ED15  f6=0x34D8ECF5  f7=0   (all constant)

That is a serialized array of identical structs with self-referential offsets.
It carries NO per-character data. The pattern I built the interpretation on was
the record stride.

## RETRACTION 2: NORM is a generic container, not a font format

`keyboard1.bin`, `model.bin`, `font1.bin` and `font1.dat` all open `NORM`, and
`font1.dat`'s payload at 0x20 is `MDLB` -- model blocks. So the character->glyph
chain is not sitting in a font file waiting to be read.

## RETRACTION 3: "parse it offline, no game run needed" was wrong

Two reasons. The format is a general asset container the runtime decodes, and
my own arithmetic on `jetfont.dat` does not close: its directory entry says
0x40020 per block while `(len-16)/6` says 196,656. Those disagree, so the
format reading is unreliable.

AND THERE IS NO TEXTURE-IMAGE DUMP IN THE TREE. Enumerated:
`RECOMP_FB_DUMP`, `RECOMP_FB_DUMP_DRAW`, `RECOMP_FB_DUMP_FLIP`,
`RECOMP_FB_WATCH_DUMP`, `RECOMP_FF_BATCH_DUMP{,_AFTER,_TEX}`,
`RECOMP_FF_DUMP`, `RECOMP_OBJECT_DUMP`, `RECOMP_TEXTURE_SLOT`,
`RECOMP_FMV_DUMP`, `RECOMP_AUDIO_DUMP`. None writes a bound texture out. So
matching a photographed corrupt shape against the atlas needs a NEW
instrument; it is not a switch away.

## The measurement that matters next, and why the existing watcher may be blind

`ff_watch_vertex` stores `inputs[0]` and `inputs[9]` (`nv2a_pb_exec.c:781`), so
it has position and texcoord0 -- but:

  1. It is a CHANGE detector: it prints only when a batch differs from the
     previous draw of the same batch ordinal, and only the first differing
     vertex. It cannot say WHICH atlas cell a malformed letter sampled.
  2. It needs the atlas address up front (`RECOMP_FF_BATCH_WATCH_TEX`, hex,
     exact match on `NV097_SET_TEXTURE_OFFSET`). paths.conf has had it set to
     `01737000` with a comment that it "was never drawn in four runs"; the
     player's 13:29 session DID draw it (19 batches, 27,895 draws), so that
     comment is stale. Gameplay six-vertex batches were seen on `01727000`,
     one hex digit away -- but those turned out to be a STATIC HUD QUAD
     (identical pos and t0 every frame), not glyphs.
  3. BOTH call sites (`nv2a_pb_exec.c:4026`, `:4060`) are inside the
     fixed-function branches. Neither is on the programmable vertex-program
     path.

MEASURED, and this is the substantive new result: a 24-second gameplay window
(t=100.02-124.11, replayed from the player's recording under the player's own
switch set) captured 96,207 fixed-function batches and contains NO GLYPH-LIKE
QUADS AT ALL. No texture in it shows more than 4 distinct texcoords, and the
common vertex counts are 4,176 / 3,471 / 9,609 -- world geometry. Over the same
window the programmable path ran 293,224 VSH batches and 731,012 GPU vsh draws,
none of which `ff_batch_dump` or `ff_watch_vertex` can see.

THE CAVEAT THAT KEEPS THIS HONEST: there is no positive control that a dialogue
box was on screen during t=100-124. "No glyph batches on the FF path" is
therefore consistent with "text is on the programmable path" AND with "no text
was being drawn". Settle that before building anything -- a framebuffer dump
timed to the same window costs one replay and would supply the control.

## What the next instrument has to do

The chain that separates the three possibilities is
character code -> table entry -> glyph index -> atlas rectangle -> extracted
glyph image. Of that, the tree can currently observe none of the middle. The
minimum useful addition is a per-draw capture on the PROGRAMMABLE path that
records the texture offset and the per-vertex texcoords of a small screen-space
quad, plus a one-shot dump of the bound texture. With both, a correct and a
corrupt rendering of the same letter can be compared directly: identical
texcoords isolate it to sampling or upload, differing texcoords to selection.

## Unrelated, and recorded so it is not lost

The 140,801 dropped draws in the player's session (`combiner output mode`,
`nv2a_texture_copy.c:28`) DID NOT REPRODUCE under replay -- `rejected=0` past
t=257 with the player's full switch set. The remaining differences are audio
forced off and the possibility that it is content-gated beyond what 257 seconds
of replay reached. That counter says nothing about text; it is a lead for the
spray cans and the wall tags.
