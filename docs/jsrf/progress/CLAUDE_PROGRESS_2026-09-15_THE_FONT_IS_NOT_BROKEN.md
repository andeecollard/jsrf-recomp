# The text is not broken; that is what JSRF's glyphs look like

15 September 2026. Written as a negative result, because the next person to see
a screenshot of `DDD1 TRICKS` will otherwise spend the same day on it.

## The reports

A person playing sent three screenshots of what looked like glyph substitution —
characters rendering as *other valid glyphs from the same font*, cleanly drawn:

| seen | expected | where |
|---|---|---|
| `DDD1 TRICKS` | `0001 TRICKS` | HUD trick counter |
| `x DD` | `x 00` | HUD spray-can counter |
| `%SOU` | `$SOU` | HUD |
| `Tr§ jumping up onto r` | `Try jumping up onto r…` | tutorial message |
| `ollect 10 Spra§ Cans!` | `Collect 10 Spray Cans!` | mission message |

## The answer: no defect

All three substitutions are the shipped art, confirmed by decoding the assets
and looking at them.

- **The HUD's `0` is a D-shape.** `Media/Disp/SprNorm.dat` chunk 0.0 is a sprite
  sheet, not a character-indexed font: two sizes of the digits 0–9 plus word
  sprites (`TRICKS`, `pts`, `LAP`, `GET No.`, …). Its zero is a heavy filled
  rounded square with a small triangular notch cut into the upper-left corner.
  At HUD size it reads as `D`. The `1` beside it reads as `1`. `DDD1 TRICKS` is
  what a correctly drawn `0001 TRICKS` looks like in this game.
- **jetfont's `y` has a flat horizontal descender**, drawn as bars under the
  bowl rather than a tail. At message size that silhouette reads as `§`.
- **jetfont's `$` has two rectangular counters** either side of a heavy blocky
  S — the same skeleton as `%`, and at HUD size the two are barely separable.

`Media/Font/jetfont.dat` pages 4 and 5 hold the half-width Latin face, ordered
ASCII with holes: cells 0–58 are `0x21`–`0x5B`, then `0x5D`–`0x75`, with `0x5C`
(backslash) absent — it is ¥ in Shift-JIS and lives on a full-width page.

## Why the "per-font index bug" theory was wrong

The theory was that `0` renders correctly in the message font (`10` in "Collect
10 Spray Cans!") and wrongly in the HUD font, so some per-font glyph mapping
must be at fault. Two different fonts with two differently-shaped zeros explains
the same observation with no code involved.

**The fact that should have killed it earlier was already in hand.** The errors
are selective *within one string drawn by one call*: `Spra§ Cans` has a correct
`a` before the bad glyph and `C a n s` after it, same formatter, same draw, same
texture. A fault in a shared index or UV computation moves every character in
the string. A per-character fault would need exactly one entry of one table
corrupted per font and nothing else.

Two further measurements close the renderer-side alternatives:

- Both font textures are DXT3 with **`mipCount = 0`**. There is no LOD chain, so
  every wrong-mip theory is excluded by construction.
- Cell pitches are ≈21.3 × 34.1 texels on the 256 pages and ≈26 on the 512
  pages — **not powers of two** — so no power-of-two addressing error lands
  exactly one cell away, and a pitch or swizzle fault shears a region rather
  than substituting one clean glyph.

## Adjacency, since it was the first thing asked

Within jetfont page 4/5: `$`→`%` is +1, `0`→`D` is +20, `y`→`§` is +3. No single
index arithmetic produces all three — which was the first sign the premise was
wrong rather than the arithmetic.

## Not established

- **What draws `$SOU`.** It is not in the HUD sprite sheet, which has no `$`, no
  `%` and no loose letters. The `$` glyph shape accounts for the appearance, but
  the drawing path was not identified.
- **The dropped leading `C` of "Collect".** Nothing was found bearing on it, and
  nothing here is built on it. It may simply be a message caught mid-animation.
- **The guest's glyph lookup was never located.** `jetfont.dat` holds textures
  only — no metrics, no character map — so the mapping lives in the executable,
  and the path string at VA `0x001C5B50` has no code xref: the font is loaded by
  a table-driven resource loader. Searches for the mapping table implied by the
  atlas holes (ascending index runs, character-code lists including the
  distinctive `0x59,0x5A,0x5B,0x5D` that skipping backslash would produce) found
  nothing, so the mapping is arithmetic in code rather than a table. **This is a
  "not established", not a clean bill of health** — nothing here reports for or
  against `ACCURACY_GAPS.md` §8's open shift defects on that path.

**Do not chase `CMGameGLFont::draw`.** `CLAUDE_PROGRESS_2026-09-10_OPENING.md`
records a probe with a positive control showing `CMGameGL::setRenderState` is
never called across a run that drew 102,000 batches. `CMGameGL` is not the retail
path. `TNRoman.bin` fits the same picture: 352 records of
`{float u, float v, float width, 0}` with `u` quantised to n/127, loaded inside
D3D device init — a debug overlay font.

## Not the same as the intro fade

The intro card defect is a continuous per-vertex colour lost before the vertex
buffer; glyph identity is a texture coordinate on a different path. More
decisively, the intro failure is *uniform* — the whole quad arrives at alpha
255 — whereas here neighbouring characters in one string are correct. Nothing in
this note touches STATUS.md's open question of whether that path carries other
tints.

## Tooling kept

`diagnostics/jsrf_first_fault/decode_norm_asset.py` decodes any
`Media/**/*.dat` to PNG, documenting the NORM/MULT container and its DXT3
texture payload. The one trap is that nested containers use offsets relative to
their own base, so a walker that assumes file-absolute offsets works on
`jetfont.dat` and breaks on `SprNorm.dat`.

## If it is ever worth a runtime check

`RECOMP_DRAW_CAPTURE=<prefix>` with `RECOMP_DRAW_SAMPLE=<draw#>`
(`src/kernel/nv2a_pb_exec.c:2506`) writes all 16 vertex-shader outputs per
vertex, i.e. the exact UV rect a text quad sampled; `RECOMP_TEXTURE_SLOT`
identifies which slot holds the font. For a 256 page a correct cell is
`u ∈ [col/12, (col+1)/12]`, `v ∈ [row/7.5, (row+1)/7.5]`. That single capture
separates a guest-side index fault from a renderer-side sampling fault, if a
report ever survives being compared against the art first.
