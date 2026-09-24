# JSRF goals — every scene is reachable, 24 September 2026 (night)

Supersedes `JSRF_GOALS_2026-09-23_LATE_LIFT_AS_MUCH_AS_WE_CAN.md` for
ORDERING. That file keeps the record of G49–G56 and the 24 Sep daytime work
(fog, the city, the drop audit, the frame rate); nothing here renumbers it.

**The player's direction, 24 Sep (night):**

> I think today progress has been unnecessarily slow. Are we using the
> symbols/decomp/lifters enough?

The answer changed how the night went. Until then every question about
Rokkaku-dai cost the player a play session. By the end of the night, any
chapter, mission or cutscene can be reached unattended in about a minute,
and a glitch can be recorded frame by frame while it happens.

## What the player can see, now

| defect the player reported | state tonight |
|---|---|
| characters whited out in Rokkaku-dai (hair, glasses, skates, face) | **fixed**, confirmed by the player (G58) |
| Rokkaku-dai city missing | fixed earlier today (G54, c8851f0) |
| cutscene objects "disappearing temporarily" (DJ K, police, crows) | **root cause fixed** for the DJ K scene (G57); sweep of all cutscenes running (G61) |
| music dies after ~2 minutes | **fixed**, player + logs: stage track streams in real time to the end of 23-minute sessions (DSOUND lift, 23 Sep) |
| Roboy's graffiti studio cannot paint | **fixed in code** (G59), awaiting the player |
| Rokkaku-dai water is a see-through void | **bump mapping implemented** (G60), awaiting the player; may need an xemu reference |
| "is the game progressing?" | **yes**: the save shows 16 Poison Jam tags still to cover in Rokkaku-dai (11–14, 23–34); Chuo Street comes before the Poison Jam chase |
| Poison Jam cutscene missing elements | open (G63) |
| boost flicker | open; untested since G57 |
| corrupt text in speech boxes / trick names (G2) | not worked tonight; the e032 subtitles recorded clean |

## Done tonight

**G57 — `frndint` ignored the x87 rounding control.** The lifter emitted
`rint()`, which rounds to nearest whatever `fldcw` set, so the game's MSVC
`floor()` (0x17C61D) and `ceil()` (0x17C54F) both rounded to nearest. The
skeletal sampler 0x5F0F0 then read one key past its table once per animation
loop: bone matrices of 1e20–1e30, and the whole scene gone for one frame in
every 60. Fixed in the lifter (4778f4e, `recomp_frndint`) and, until the gen
tree is regenerated, by a run-time lift of `_frnd` 0x17EED5 (38abc8e,
`RECOMP_FRND_LIFT=0` disables). DJ K replay: 11 transient frames → 0.
**Pending: the player's decision to regenerate** (then delete x87_lift.c).

**G58 — 16-bit swizzled textures on the GPU (81d08f0).** SZ_X1R5G5B5 (0x03)
and SZ_A4R4G4B4 (0x04) fell through the Metal shader to linear R5G6B5 at
unnormalised coordinates. Beat's shading texture on stage 1 is A8R8G8B8 in
the Garage and A4R4G4B4 in Rokkaku-dai — the whole "same character,
different level" mystery. GPU-vs-CPU test with negative control.

**G59 — linear 32-bit textures (cee2ade).** The graffiti editor's canvas,
brush and palette are LU_IMAGE_A8R8G8B8 (0x12); the gate refused them and
dropped every draw (player log: 894 of 14,911 batches). 0x12 and 0x1E now
take the linear path in gate, CPU sampler and shader.

**G60 — BUMPENVMAP (e8987bc).** Modes 6/7 implemented after xemu's psh.c,
matrix word order cross-checked against the game's own
`D3DDevice_SetTextureState_BumpEnv`. `RECOMP_TEXMODE_BUMP` (default on);
`RECOMP_MARK_BUMP` / `RECOMP_MARK_BUMP_ENV` paint approximated / displaced
draws magenta / cyan.

**G62 — reachability and evidence tools.**
- `RECOMP_CHAPTER_SELECT` / `RECOMP_CHAPTER_JUMP=<chapter>:<mission>`
  (a987c6c, be48155): KeybadeBlox's ChapterSelect as a run-time lift, and an
  unattended jump from the Garage. The mod's header has ClearStateFlags at
  0x128C0, which is CActMan::GetAction; the real one is 0x3AE20 — ours is
  fixed, the mod still has the bug (worth reporting to KeybadeBlox, by the
  player: the decomp refuses LLM-written contributions).
- `RECOMP_FLIGHT_FRAMES` + M (eef8a5c, 4282199, 12e6abb): last N presented
  frames and every draw, written on the pad mark; `flight_diff.py`. Frame-N
  pixels come from draws-(N−2): the snapshot is taken at FLIP_STALL, before
  the swap's copy quad.
- `RECOMP_STAGE_MEM` (6b9a835): guest memory at harness captures.
- `gametools/` (c106539): save decoder/editor with byte-for-byte round trip,
  mission and event parsers, launchers.

## Refuted tonight — read before theorising

- Beat's whiteness from fog, the hardware sampler, bump mode 6, or
  fixed-function colour material/specular: each tested and refuted. The
  `RECOMP_FF_MATERIAL` switch (fc4adeb) stays in, off, unproven either way.
- The see-through floor from the depth-range cull (`RECOMP_LEGACY_ZCLAMP`):
  refuted; it was the water, and the water is drawn where it should be
  (`RECOMP_MARK_BUMP`, magenta over the whole void).
- The DJ K dropout from surface-cache thrashing or skipped syncs: every
  counter flat in the glitch flips; it was G57.
- DXT3 glyph corruption from DXT1's c0 ≤ c1 rule: our decoders are already
  4-colour for DXT3.

## Open, in order

1. **G61 — sweep every cutscene** with `RECOMP_GLITCH_WATCH` (in-engine
   transient detector) across the chapter intros N:96 and every event the
   catalogue says plays on a jump; triage each hit. In progress.
2. **Player checks:** graffiti studio (G59), water (G60), cutscenes (G57).
3. **Regenerate** the gen tree with the G57 lifter (player's decision), after
   copying the current tree aside (memory: the gen tree is overwritten in
   place).
4. **G63 — Poison Jam missing elements** (e034/e111, mssn0242): reachable via
   the `pj-e034` checkpoint plus a talk and the chase; the only event-local
   model is `e_stationsaku` (DXT3, supported), Poison Jam come from the
   player-character data.
5. **Boost flicker** — re-test after G57; a mission-byte switch (GG-Notebook
   mission_bin.md l.81) reduces boost effects for an A/B.
6. **G2 text** — resume from the handovers; the glyph atlas is DXT3/DXT5-like.
7. Carried unchanged from the 23 Sep (late) file: G49–G56, the frame rate,
   the D3D lift.

## Housekeeping owed (player's decision)

- 16+ local commits on `jsrf/g2-text-clobber-and-the-symbol-layer` are not
  pushed.
- Merged branches from this morning and three agent worktrees under
  `.claude/worktrees/` can be deleted.
- `docs/jsrf/STATUS.md` was last measured on 14 Sep.
