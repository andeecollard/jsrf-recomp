# G2: the string is perfect, the pages are bound, and the page boundary is the trigger

20 September 2026, evening. The player played a live session with sound, hit the
graffiti tutorial, and photographed the corrupt banner. The harness read the
guest's own string at that same moment. Two standing hypotheses die here.

## 1. The corruption is renderer-side. Proven, not inferred.

At the frame the player photographed, the text node held:

```
$x314 $n$x308<8140>$n$x308<8140>$n$x308<8140>$n$x308<8140>$n$x308<8140>
$n$x120Collect 10 Spray Cans and perform a$n$x128$c5Boost Dash$c1 with the
$c5<8167>B button<8168>$c1!
```

Drawn as `llect 10 Spra$ Cans and perform a` / `Boost Dash With the "B button" !`.

**The string in guest memory is whole and correct.** G2's central question is
answered: nothing the guest produces is wrong, and the fault is entirely in how
we draw it. Evidence kept in `/tmp/jsrf-g2-evidence/` — the expected string, the
object dump and the frame, all under one command id.

## 2. The MECHANISM "a page-1 character samples a CJK sheet" is wrong

The 19 Sep entry located the root cause as the second Latin page being
mis-bound, with page-1 characters sampling a 512x512 CJK sheet. Measured in
code with `RECOMP_FONT_TRACE=1`, that does not hold:

- **Both Latin pages are bound and drawn**, interleaved 16 draws of page 0 then
  3 of page 1, repeating. Page 1 is not "never bound".
- **Both are 256x256 with the correct format.** `fmt=08870E29` decodes to log2
  width 8, log2 height 8, 7 mip levels. The 512x512 sheets carry `09980E29`,
  log2 9/9, 8 levels — so the guest asks for those sizes and our decode agrees.
- **Every font page holds distinct, stable content.** FNV hashes of each page's
  level 0: nine distinct addresses, nine distinct hashes, and each address hashes
  the same on every draw. No page is a copy of another and none changes.

**A size correction that would have blocked the old test.** The 19 Sep note says
a Latin page is 65,536 bytes and a CJK sheet 262,144, so "any text draw sampling
262,144 bytes is on a CJK sheet". The real figures are **87,376 and 349,520** —
4/3 of those, because `nv2a_texture_copy_texture_bytes` counts the whole mip
chain and these have 7 and 8 levels. The old test would never have matched.

## 3. The speech bubble renders CORRECTLY, on both render paths

Corn's dialogue captured at the same scene, both arms, same frame:

| arm | drawn |
|---|---|
| `RECOMP_METAL_FF=1` (the player's path) | `This is the GG's Garage. / Hey, where's our pizza?` — correct |
| `RECOMP_METAL_FF=0` (CPU fixed-function) | correct |

`where's` contains `w` = 0x77, a page-1 character, and it is drawn correctly on
both paths. So the font page machinery — selection, binding, UVs — works.

## 4. The markup theory was mine, and it is DISPROVEN

For an hour this document argued that `$c` directives and Shift-JIS `0x81 xx`
sequences desynchronised the glyph walker, because the corrupt banner had them
and Corn's clean bubble did not. Replaying the player's own session produced the
control that kills it:

| string (same font, same scene, same markup) | page-1 chars | drawn |
|---|---|---|
| `$x314 $n$x308<8140>x5 $n$x254$c5Grind a rail$c1!` | **none** | **correct** — every character, no leading loss |
| `...$x120Collect 10 Spray Cans and perform a$n$x128$c5Boost Dash$c1 with the $c5<8167>B button<8168>$c1!` | `y`, `w` | corrupt — `llect`, `Spra$`, wide `w` |

Identical markup: both open `$x314 `, both carry five `$n$x308<8140>` blank
lines, both use `$c5`/`$c1`. The only difference is whether the line contains a
character at or above 0x76. **The 19 Sep page-boundary finding is right and this
document was wrong to set it aside.**

The leading-character loss tracks it too: the line with no page-1 character
loses nothing. So "leading characters vanish" and "page-1 glyphs are wrong" are
one defect, not two.

## 5. What survives from the code measurements

The trace still refutes the *mechanism* the 19 Sep entry proposed, even though
its trigger was right:

- both Latin pages are bound and drawn, interleaved 16 draws to 3;
- both are 256x256 with the correct format and 7 mip levels;
- every font page holds distinct, stable content by hash.

So nothing is mis-bound or mis-sized. A line needing both pages genuinely is
drawn in two passes. The fault is in how those passes are split or ordered --
and when the second pass is required, the START of the line disappears.

One further narrowing, from the same evening: Corn's bubble draws `where's`,
containing page-1 `w`, **correctly**, on both render paths. So the bubble font is
unaffected and the banner font is not, which matches the 19 Sep note that each
text element uses a different font and a different draw path.

## Next

Both test strings appear in one replay of `padrec/jsrf-2026-09-20_195259-54912`
(gen `46bb115cdb053f4e`, matching), so the A/B needs no player and no new
session. `Grind a rail!` draws correctly and `Collect 10 Spray Cans...` does not,
in the same font at the same scene.

Trace the glyph quads for each and compare how the line is split into passes:
where the page-0 pass ends, where the page-1 pass begins, in what order they are
submitted, and which cell index each quad carries. The defect is in that split,
because when a line needs the second pass its opening characters vanish as well.

The route is also saved as 200 world-coordinate waypoints in
`/tmp/jsrf-tutorial-route.json`, which survives a regeneration that would retire
the recording.

`RECOMP_FONT_TRACE=1` is opt-in and read-only: for every DXT3 square texture of
256 or 512 it prints size, resolved address, vertex count, the raw
`SET_TEXTURE_FORMAT` / `IMAGE_RECT` / `OFFSET` methods, mip level count and an
FNV hash of level 0.
