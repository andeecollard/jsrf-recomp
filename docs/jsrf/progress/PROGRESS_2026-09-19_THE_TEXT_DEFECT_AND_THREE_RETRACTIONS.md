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

---

# ADDENDUM: the positive control exists, and it splits the two symptoms apart

Taken after the above, from framebuffer dumps of the player's own 13:09-13:29
session. Preserved in
`glyphdump-2026-09-19-1329-PLAYER-SESSION-KEEP/` with a README (178 frames,
indices 063-240).

## FIRST, DAMAGE I CAUSED, so the next person checks

`paths.conf` line **479** carries an ACTIVE
`export RECOMP_FB_DUMP=".../glyphdump/"`. The commented-out line at 199 is
superseded by it. The dump sequence RESTARTS AT 000 every run, so the replays
I ran at 13:43-13:56 -- which sourced paths.conf for fidelity -- overwrote the
low indices of the player's session. Frames below 063, roughly 13:09-13:14, are
GONE. Those covered the 13:11-13:12 dialogue captures. They survive only as the
player's own screenshots.

There is a memory note saying exactly this ("every run overwrites the
RECOMP_FB_DUMP images; preserve first") and I did not apply it when sourcing
someone else's config. Reading line 199 and stopping was the error: grep the
WHOLE file for a later active export of the same variable.

## The positive control, which the earlier window lacked

Text IS rendered and IS captured. `report150` shows
`Pull the "Right Trigger" near / the round-shaped mark to tag!` rendering
CORRECTLY, digits included (`x 16` in the HUD). So "no glyph quads on the
fixed-function path" is now a real finding about the path and not an artefact
of nothing being drawn -- though it still wants the two measurements taken in
the same window to be airtight.

## The corruption is STABLE, not transient

Frames 205, 207, 209, 210 and 212 all render the same line identically:
`llect 10 Spra$ Cans and perform a`. Same two characters missing, same corrupt
glyph where `y` belongs, every frame. An earlier suggestion of mine that the
corruption might be per-frame or racy is NOT supported.

## THE TRUNCATION IS NOT A RENDERING FAULT

Measured, near-white text pixels, line 1:

    report212 (truncated)  x = 147 .. 515   (368 px wide)
    report150 (correct)    x = 149 .. 489   (340 px wide)

Both begin at the same left margin, and the LONGER line does not begin further
left -- so the text is LEFT-ALIGNED at x~148, not centred. And there is no gap
at the left of the truncated line: the `l` sits at the box edge, so the layout
never reserved space for the two missing characters.

Three consequences:

  - MY LEFT-EDGE CLIPPING HYPOTHESIS IS REFUTED. The line has a ~148 px left
    margin and is not against the screen edge; nothing is being clipped.
  - MY "ONE BUG" HYPOTHESIS IS REFUTED. It required a wrong glyph to carry a
    wrong advance, mis-measuring the line and pushing it off the edge. There
    is no centring and no clipping, so that chain cannot run.
  - The string the renderer lays out genuinely BEGINS at `l`. The leading
    characters are absent from the string or the iteration start, not lost
    in glyph selection, sampling or upload.

So the two symptoms are most likely TWO DEFECTS:

  - TRUNCATION -> string source or iteration start. Look at the string-table
    lookup and whatever formats it, not at the atlas. Note the loss is a
    VARIABLE prefix (`Collect`->`llect` 2, `Not`->`ot` 1,
    `Farside Stab Soul`->`de Stab Soul` 5), so a constant pointer bias does
    not explain it on its own.
  - SUBSTITUTION -> still open, still the glyph pipeline, and still needs the
    programmable-path capture and an atlas dump described above.

## Also captured, and it corroborates the player

The spray-can HUD icon renders as a SOLID BLACK silhouette in report150 and
report207, with the count beside it (`16`, `19`) rendering correctly. That is
the player's "spray cans not rendering" reproduced in preserved evidence rather
than in a screenshot, and it sits beside the 140,801 `combiner output mode`
draw refusals as the leading candidate for the same cause.

---

# ADDENDUM 2: chasing the truncation to the string source

## The strings are found, and the exact one is known

They are NOT in `List_eng.dat`. They are plain text inside
`Media/Mission/mssn*.bin`, mixed ASCII and Shift-JIS, with `$`-escapes. The
tutorial set is in `mssn0101.bin` (and duplicated in `mssn3400.bin`).

The line the player photographed is at `mssn0101.bin:0xBF26`, and in full:

    \0 $n\x81@$n\x81@$n\x81@$n\x81@$n\x81@$n
    Collect 10 Spray Cans and perform a
    $n$c5Boost Dash$c1 with the $c5\x81gB button\x81h$c1!$r$e$r\0

`$n` = newline, `$c<digit>` = colour, `$r`/`$e`/`$a` = other controls,
`\x81\x40` = the SJIS full-width space, `\x81\x67`/`\x81\x68` = SJIS quotes.
So the intended render is
`Collect 10 Spray Cans and perform a / Boost Dash with the "B button"!`

Note the structure: FIVE `$n` + full-width-space pairs, then a BARE `$n`
immediately before `Collect`.

## What the frames show against that source

    line 1   Collect 10 Spray Cans and perform a   ->  llect 10 Spra? Cans and perform a
    line 2   Boost Dash with the "B button"!       ->  Boost Dash With the "B button" !

LINE 2 IS COMPLETE. Only line 1 loses characters, and both lines come from one
string in one record. So the truncation is PER-LINE, not per-string, and
whatever drops the characters runs after the newline split.

The difference between them is what sits between the `$n` and the text:
line 1 is `$n` + `Collect`, line 2 is `$n` + `$c5` + `Boost`.

## Not clipped: the glyph is whole

At 7x magnification the first surviving character of line 1 is a COMPLETE `l`
with clean background to its left -- no sliced pixels. Across three affected
strings the losses are 2, 1 and 5 characters, all landing exactly on glyph
boundaries; a scissor rectangle would have cut through at least one. Combined
with the left-margin measurement in addendum 1, both clipping explanations are
dead and the characters are genuinely never laid out.

## The parser could not be found statically, and the searches are validated

Four searches, each with a positive control, because three of them returned
zero and a zero from a broken search is worth nothing:

  - `$`-escape dispatch as a compare chain: 9,019 functions scanned. 11 compare
    against `0x24`, 14 against `n`/`c`/`r`/`e`, NONE does both.
  - The 11 `0x24` functions are not character tests. The best-shaped candidate,
    `sub_0002E9D0` (466 bytes, byte loads, 7 jump tables), uses it as a SWITCH
    RANGE BOUND: `cmp ecx, 0x24; ja default` over 37 cases, dispatching through
    a byte index table at `0x2EBB8` and a jump table at `0x2EBA4`.
  - 8-bit compares exist (53 in one file alone) but NONE against `0x24`
    anywhere in the tree.
  - A first attempt found "only immediates 0-8 exist", which was MY REGEX:
    `(-?\d+|0x[0-9A-Fa-f]+)` matched the leading `0` of `0x10` because the
    decimal alternative came first. Fixed before any conclusion was drawn.

So there is no `$` character test in the generated code at all. The escape
handling is table-driven, or happens somewhere a comparison search cannot see.
Static hunting is exhausted.

## The decisive experiment, and it needs one small instrument

Search guest RAM for the TRUNCATED byte sequence while the line is on screen:

  - if `llect 10 Spray` exists in RAM, a guest routine built a truncated copy
    and the defect is in string processing;
  - if only `Collect 10 Spray` exists, the string is intact and the loss is in
    the renderer's iteration or layout.

That single bit decides which half of the pipeline to open, and nothing in the
tree can do it today: `RECOMP_DUMP_VA` prints dwords at addresses you already
know, and there is no pattern search. The instrument is small -- scan the guest
RAM range for a supplied byte string, print hits with their addresses -- and it
would serve every future "did the guest or did we?" question, which this
project asks constantly.

---

# ADDENDUM 3: the instrument, and the answer it gave

## RECOMP_RAM_FIND

`diagnostics/jsrf_first_fault/ram_find.{c,h}`, pure functions in the shape of
`wild_ptr.{c,h}` beside them, driven from the periodic report in `main.c`.
`RECOMP_RAM_FIND=<pattern>[;<pattern>...]`, plain text with `\xNN` escapes;
`RECOMP_RAM_FIND_AFTER=<seconds>` delays the first scan. Read-only and off
unless set. Both are value-carrying and registered as such in
`switch_audit.py`. New ctest `jsrf_ram_find`; suite is 79/79.

EVERY BLOCK CARRIES A POSITIVE CONTROL. Eight bytes are read out of guest
`.text` at `00011000` and searched for on the same pass. If that reads 0 the
block says "SCAN IS DEAD, every count below is void". The instrument exists to
make ZEROS meaningful and a zero from a scan pointed at unmapped memory is
indistinguishable from a real one -- this tree has drawn that false conclusion
before.

The test leans on the negative cases, including the trap that matters: a search
for `llect 10 Spray` matches INSIDE `Collect 10 Spray`, so an unanchored search
for a truncated string proves nothing. `ram_find_test.c` asserts that directly.

Cost, measured rather than claimed: 127 MB, three patterns plus the control,
**19-22 ms**, once per report interval.

## THE ANSWER: the guest's string is intact, so the truncation is OURS

29 scans over a replay of the player's session, every one with a passing
control:

    27 scans   "Collect 10 Spray"        1 hit at 00C2EF1B
    27 scans   "llect 10 Spray"          1 hit at 00C2EF1D   <- +2, INSIDE it
    27 scans   "Spray Cans and perform"  1 hit at 00C2EF26
     2 scans   (boot, before the mission file was loaded)  0 hits

Exactly ONE copy of the line exists in guest RAM and it is COMPLETE. The
truncated form is found only as a substring of the intact one, at exactly +2.
No guest routine ever built a short copy.

So the missing characters are lost on OUR side, in the renderer's iteration or
layout -- not in the guest's string handling. That closes the question addendum
2 could not, and it means the string-table hunt is over: the string was never
the problem.

THE LIMIT, stated: scans are one per report interval, so a copy built and freed
entirely between two scans would be missed. A buffer persisting while the line
is on screen would not, and the line is displayed for far longer than the
interval.

## And one observation from the player, watching the replay

"lines above the letters and flickering". Thin artefacts along the top edge of
the glyphs is ATLAS BLEED -- sampling a texel row outside the glyph's cell and
picking up its neighbour. That is direct evidence for the UV/addressing branch
of the substitution defect rather than the selection branch: a fractional
overreach bleeds a neighbouring row, while a whole-cell error would swap the
glyph outright. Both symptoms may be the same addressing error at two
magnitudes, which is now a testable claim rather than a guess.

---

# ADDENDUM 4: the text is ANIMATED, which retracts "stable"

## The retraction

Addendum 1 says the corruption is "STABLE, not transient", on the strength of
five `report*.bmp` rendering one line identically. THAT WAS THE WRONG
TIMESCALE. `report*.bmp` is written ONE PER REPORT, so those five frames are
tens of seconds apart, not adjacent. They show the line was static once
settled; they cannot see frame-to-frame behaviour at all, and the claim was
never tested where it mattered.

The player, watching a replay: "lines above the letters and flickering",
"Gum also glitches - its animated", "Corn's pizza line also glitches".

## The evidence that settles it

`fbdump/report018.bmp` catches a speech box MID-REVEAL:

    rendered:  The ne<corrupt>t technique
    source:    The next technique is a Grind.

so the box is typed out progressively rather than drawn whole. The corrupt
glyph is the `x`, which is the SAME instance
`PROGRESS_2026-09-19_THE_GLYPH_DEFECT_IS_PHOTOGRAPHED_AND_G2_IS_WRONG.md`
recorded at 04:13:36 -- it reproduces exactly.

And in the same frame the soul banner reads `arside Stab Soul`: ONE character
lost, where the same banner lost FIVE in the 13:29 session. THE TRUNCATION
VARIES OVER TIME FOR THE SAME STRING. It is not a fixed offset, which is what
addendum 1 implicitly assumed and what the variable 2/1/5 losses were already
hinting at.

Both of those are consistent with the missing characters being a function of an
ANIMATION's progress rather than of a layout or lookup constant. That does not
contradict addendum 3 -- the string in guest RAM is intact and whole, measured
29 times -- it just means what we get wrong is WHICH SPAN of that intact string
we draw, and that the wrong span moves.

## The path question is STILL open, and this run did not settle it

200,000 fixed-function batches were captured over t=170.00-253.87, 85 distinct
textures, and the widest texcoord spread on any of them is SIX distinct pairs
(`01737000`, in 6/15/18-vertex batches). Nothing with the many-distinct-texcoord
signature of glyph rendering. Only two textures sit below 0x01000000: the
framebuffer `005F0000` and `00DCC000`.

BUT THE POSITIVE CONTROL FAILS FOR THAT WINDOW. Of 55 full-size framebuffer
dumps, `report043`-`report050` contain no bright pixels at all: there was no
text on screen for most of t=170-250. Text returns at `report051` (+255 s) and
`report052` (+260 s), which is exactly where the 200,000-line cap ran out. So
"no glyph quads on the fixed-function path" is once again a statement about a
window with nothing to find in it.

THE TARGETED RUN THAT WOULD SETTLE IT, now that the timing is known: capture
with `RECOMP_FF_BATCH_DUMP_AFTER=250`, which puts the window on the speech box
at +255 s, with framebuffer dumps alongside as the control. If a texture with
many distinct texcoords appears there, text is fixed-function and the existing
batch watcher can see it; if it does not, text is on the programmable path and
the watcher is structurally blind, as addendum 1 suspected.

## Incidental, and useful

`jetfont.dat` is resident in guest RAM at `00CAC842` (file offset 0x882), and
read the SAME in two separate runs -- so texture offsets really are stable
across runs, as paths.conf's own note claims. `font1.dat` is NOT resident at
all: searched for and found zero times, with the control passing. Whatever
draws the glyphs, it is not reading font1.dat's file image.
