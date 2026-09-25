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
| cutscene objects "disappearing temporarily" (DJ K, police, crows) | **fixed** (G57): the 37-jump sweep shows every police and DJ K dropout gone with the fix and back without it (G61); a separate one-frame camera pop remains in 4 scenes (G65) |
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

1. **G61 — cutscene sweep: done** (see Progress, 25 Sep). Follow-ups: G65
   camera pop (xemu), G66 intermittent hangs.
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

## Progress, 25 Sep (early morning)

**G61 — the cutscene sweep is done** (e3a4907 `RECOMP_GLITCH_WATCH`; launcher
and triage scripts in `gametools/harness/`, 807bb0b). All 37 jumps of the
catalogue's class-A list, rounding fix on: **no fault, no crash**.
- **The player's "disappearing" is G57 everywhere.** Controls with
  `RECOMP_FRND_LIFT=0` → on: 2:96 e210 11→0, e213 (police) 1→0, e214 11→0;
  1:96 e200 19→1, e201 (police) 7→0, e206 15→0. With the lift off, Hayashi
  and the officers vanish for a frame; with it on, e213 and e025 recorded in
  full show every model the event file lists.
- **Left with the fix on:**
  - **G65 — one-frame camera pop** (open): e200 ≈ guest frame 3792 in five
    runs, e221, and 4:62 (twice in cutscenes, once in gameplay). The world is
    drawn from another view for one frame; draw list unchanged, transforms
    finite and unchanged in the recorded frames. It sits near camera-path
    boundaries in the event data, so it may be authored; xemu decides.
  - **G66 — intermittent hangs** in 4:96 e231 and 8:13 e291 (the ending): the
    game thread spins ~1e9 kernel calls with no draws, ADX guard ~1e9
    unmatched unlocks (the known sub_001437B0 spin). 4:96 rerun played
    through. Harness runs silence audio, which may matter.
  - Authored effects, not defects: 7:96 sparks, 2:50 fade dips, 6:60 camera
    shake, 6:61 flash. Garage idle sway (3–5 frames, diff ≈ 5) before every
    jump: benign, low confidence.
- Not covered: 8:13's later events (hung), class B/C events, e054/e080 via
  6:41/7:41/8:41/9:41/9:56.

**G64 — clean-room chapter select** (fc628e2 spec, 7a89ce3 implementation,
merged). `chapter_select.c` was a port of KeybadeBlox's unlicensed
JSRF-ChapterSelect; it is replaced by an implementation written from a spec
derived only from `default.xbe` and the mission files
(`docs/jsrf/cleanroom/`), by an author with no access to the third-party code
or the old file. Interface unchanged except: default mission when omitted is
now 96 (the chapter opening, what the game itself loads after a chapter), the
mission file is checked before redirecting, and `RECOMP_CHAPTER_JUMP_MARK`
marks the first event only. Verified in game: 2:96 jump + intro recorded,
0 transients; 5:10 reaches Future Site. **Still owed before publishing:** the
old port is in the history (a987c6c, be48155 and the sweep's e3a4907 edit of
it) — rewrite those out before the first push, or get KeybadeBlox's
permission. Player's decision.

**G59/G60 merged** (6dbb270), test health merged (9978a70: switch audit back
at baseline, two stale tools tests fixed; ctest 152/154, the two expected),
documentation refreshed (ab41d8e), upstream frndint PR prepared and not sent
(c4be904). JSRF.app rebuilt from 807bb0b.

**Running:** a whole-game map (one mission per chapter/stage, ~60 targets):
loads, faults, hangs, picture, frame time.

## Progress, 25 Sep (morning): the whole-game map, and Sky Dino 3× faster

**G67 — the game map** (9a7f3b8, `gametools/GAME_MAP_2026-09-25.md` + contact
sheet; launchers `map_run.sh`/`map_batch.sh`/`map_report.py`). 64 targets —
every chapter hub and all 20 stages with free play — plus 4 follow-ups, ~50 s
of free play each, glitch watch armed. **No fault, no crash in 68 runs; every
stage reached free play; every picture sane; no texture-format refusals.**
Frame time (capped at 60 Hz by `RECOMP_FLIP_PACE`, the compiled default):
Garage 18 ms, Rokkaku-dai 18–22, Dogenzaka 22–24, Shibuya Terminal 27–30,
99th St 30–31, Skyscraper 33–37 (4:70 75–81), Sky Dino 98–110.

**G68 — texture cache thrash: fixed** (1d71f78). Sky Dino rebuilt ~110
hardware textures every frame with none changing: the 128-slot LRU was
smaller than its working set. `RECOMP_METAL_TEXTURE_SLOTS`, default now 512:
6:60 p50 **99.0 → 34.0 ms**, texture builds 53,514 → 341; Rokkaku-dai 19.4 →
19.7 ms (the linear lookup's cost; a hashed lookup would remove it).

**Open from the map:**
- **G69 — free-play hang in Sewage Facility (3:60)**, 1 of 2 runs, ~10 s in:
  frames stop, the report keeps printing, kernel calls grow slowly, pushbuffer
  not consumed, ADX lock not held — a wait, not the G66 spin. Log
  `~/jsrf-build/runs/map/runs/m0360/runtime.log` from l.51730.
- **Performance below 60 fps** in most stages: the cost is spread over vertex
  shading, submit and guest time (~8–9 ms each) plus 2–4 ms GPU sync — the
  D3D lift (G50–G52) is the lever. Skyscraper re-measure after G68.
- Shibuya 2:10 slowed to 48–158 ms late in one run (guest "rest" time); not
  reproduced in five other Shibuya runs.
- 8:90 (Gouji Tower) looks like 8:11 (stg43); check against xemu.
- Cosmetic: the jump log names briefing-card follow-ons as `mssn04481` etc.

**G66 fix merged** (f15924e: per-thread priority restore, unmatched-unlock
safety — both default on — and `RECOMP_ADX_TRACE`); unit tests incl. the exact
poisoning interleave pass; 10 in-game runs of 4:96 with the trace running.
