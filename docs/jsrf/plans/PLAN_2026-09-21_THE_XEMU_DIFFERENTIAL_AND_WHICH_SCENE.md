# The xemu differential: what it can answer, which scene, and the exact commands

21 September 2026. Written with the tooling in place and **nothing launched**.

Tools: `diagnostics/jsrf_first_fault/xemu_capture.sh`,
`diagnostics/jsrf_first_fault/gpu_stream_diff.py` (`selftest` passes).

## 0. What the oracle is, and what it is not

xemu runs the **same guest XBE** against a different GPU model. Its pgraph
trace is ground truth for **what the guest submitted** and is worthless for
anything about our host code. So a differential can conclude:

- methods on xemu that never reach us → **our pushbuffer parse is losing them**;
- the same stream on both sides → **the fault is in our backend**, which is a
  result, not a dead end. It is the difference between "the guest asked for a
  black quad" and "we made a quad black".

It cannot conclude anything about Metal, our surface cache, our write-back, or
our combiner emitter. Those are ours alone.

## 1. THE XBE IS THE SAME BUILD. MEASURED, and this was the main risk

The differential is worthless if the disc and our dump are different builds --
and there IS a second JSRF build in circulation on the bundle disc. Checked by
locating `XBEH` at the only sector-aligned offset in the ISO (0xD14AB000),
reading 2,281,472 bytes and diffing against our dump:

```
ours : ~/Library/Application Support/JSRF/game/default.xbe
       sha256 bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d
iso  : ~/Downloads/JSRF-US.xiso.iso @ 0xD14AB000
       sha256 fd19055756719893c466302809b433b785ecf5732df0441286f3f605f0f3ef9c

FOUR bytes differ, all inside the certificate:
  0x000214  ours FF 01 .. 40   iso 02 00 .. 00     AllowedMedia
  0x000218  ours 07           iso 01              GameRegion
Both: base 0x00010000, SizeOfImage 2590240, TimeDate 2002-01-28 17:47:19 UTC.
```

Our dump is a media/region-unlocked copy of the identical image. **Not one byte
of code or data differs.** Re-derive this before trusting any future
differential against a different ISO.

## 2. WHICH SCENE. Title screen first; the main menu is NOT supported by the evidence

**Capture the TITLE SCREEN.** xemu boots straight into it with no input, ours
reaches it in ~20 s, and it is the only scene where we currently have a
per-draw pixel measurement on our side to diff against
(`~/jsrf-build/fbdump-2026-09-21_1400-LOADMENU-CENSUS-KEEP`, 24 per-draw
captures: the overlay ellipse, bars and lettering shade to exactly (0,0,0) for
23 consecutive draws with correct coverage and correct occlusion). Ask the
trace three things:

1. the **combiner / output-control words** live when those quads are submitted;
2. the **texture bind** for them (`SET_TEXTURE_OFFSET` / `_FORMAT` / `_CONTROL0`);
3. `SET_BLEND_ENABLE` / `SET_BLEND_FUNC_SFACTOR,DFACTOR` and
   `SET_COMBINER_CONTROL` around the same draws.

**Do NOT use the main menu as a G2 reproducer on the strength of the
inverse-length claim.** I went looking for the evidence behind it and it is not
in the preserved captures: `glyphdump-2026-09-21-SAVEPOINT-KEEP` contains no
save screen and no menu at all -- 607 full frames of the tutorial and the
Garage from four different runs -- and none of the 111 marks shows a menu
either. The save-screen ordering is the player's eyes, once, with no capture.
Worse, it does not discriminate: the second failure mode measured below drops
exactly the characters `v w x y z`, so an all-caps heading is immune to it and
a lowercase button is not, which produces the same "headings clean, buttons
unreadable" ordering for a completely different reason.

**For G2 the matched pair is the Garage dialogue**, not a menu. It is the
documented 60-second reproducer (`docs/.../NIGHT3_..._HAS_AN_ADDRESS`, section
0a), it is the scene both preserved failure modes were measured in, and it puts
two-page Latin text on screen with a name label beside it -- which is the pair
of batches the defect lives between. It costs one dialogue trigger of input on
each side.

Order of work: **title screen first** (no input, answers the black-overlay
question, and its combiner words are the cheapest test of whether a UI quad's
state survives the trip), **Garage second** (G2 proper).

## 3. THE COMMANDS

### xemu side -- one command, and it opens a window

Title screen, structural events only (a few hundred KB, safe to leave running):

```sh
sh diagnostics/jsrf_first_fault/xemu_capture.sh \
    --preset surfaces --seconds 75 \
    --out ~/jsrf-build/xemu-title-surfaces
```

Title screen **with parameter values**, which is what the combiner comparison
needs. Keep it short; this is the hundreds-of-MB preset and the script kills
xemu at whichever of the two bounds comes first:

```sh
sh diagnostics/jsrf_first_fault/xemu_capture.sh \
    --preset full --seconds 45 --max-mb 768 \
    --out ~/jsrf-build/xemu-title-full
```

`--dry-run` prints the exact argv and launches nothing. `--audio` re-enables
sound; by default `SDL_AUDIODRIVER=dummy`, matching the muting our own harness
runs use so the two sides are not differently paced by audio.

Why the scoping is what it is: QEMU's trace machinery filters by **event name**,
never by method number, so there is no way to ask for "just the combiner
methods". The three bounds that do exist are the event set, the wall clock and
the file size, and the script applies all three and records them in
`capture.json` beside the trace. `nv2a_pgraph_method_abbrev` collapses a run of
one repeated method into a single counted line and is the right default; it
carries **no parameter values**, which is why the combiner question needs
`--preset full`.

### our side -- the matching run

```sh
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_HDD_ROOT=<disposable emulated-hdd copy> \
RECOMP_PB_SCAN=1 RECOMP_COMBINER_TRACE=1 RECOMP_PB_EXEC_TOP=400 \
RECOMP_REPORT_MS=10000 \
SDL_AUDIODRIVER=no_such_driver \
  <build>/jsrf_first_fault 2> ~/jsrf-build/ours-title.log
```

`RECOMP_PB_EXEC_TOP=400` matters: the `[GPU] 0xMMMM xN` list is **top ten by
frequency** by default, so a once-per-frame method can never appear in it and
its absence from the diff would mean nothing.

Add for the Garage run only:

```
RECOMP_GLYPH_DUMP=20000 RECOMP_GLYPH_DUMP_LATIN=1 RECOMP_GLYPH_DUMP_MAXQ=64 \
RECOMP_FONT_TRACE=4000
```

### the diff

```sh
python3 diagnostics/jsrf_first_fault/gpu_stream_diff.py selftest
python3 diagnostics/jsrf_first_fault/gpu_stream_diff.py \
    diff ~/jsrf-build/xemu-title-full/trace.txt ~/jsrf-build/ours-title.log
```

## 4. POSITIVE CONTROLS -- read these before reading any zero

- `gpu_stream_diff.py selftest` must print PASS. It feeds one synthetic line of
  every format through both parsers and asserts that the *same* combiner state
  lands on the *same* 56-word tuple on both sides. Without it, a parser that
  has quietly stopped matching reports two empty streams as perfect agreement.
- The diff prints `!! xemu side has NO method rows` and `!! our side has NO [PB]
  rows` explicitly. An empty section is never agreement.
- Counts will **not** match between the two sides; the runs cover different
  numbers of frames. Compare **presence**, and the per-flip rate where both
  flip counts exist. Our `[FRAME] flips=` is cumulative and `[FRAME-WIN]
  flips=` is windowed -- they read 31719 and 242 in the same report block, and
  the tool matches the cumulative one by name rather than taking a maximum.
- Surfaces are xemu-only here. Ours come from `RECOMP_SURFACE_CENSUS`, a
  different instrument with a different trigger. Do not merge the two lists.
