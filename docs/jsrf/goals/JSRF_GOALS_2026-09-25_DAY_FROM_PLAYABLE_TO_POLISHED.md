# JSRF goals — from playable to polished, 25 September 2026 (day)

Supersedes `JSRF_GOALS_2026-09-24_NIGHT_EVERY_SCENE_IS_REACHABLE.md` for
ORDERING. That file keeps the record of G57–G69 (the frndint fix, the texture
fixes, the cutscene sweep, the clean room, the game map, the ADX and
pushbuffer hangs) with their evidence; nothing here renumbers it.

**Where the game is.** Playable into chapter 2 on the player's save; every
chapter and every stage loads and plays unattended with no fault (68-run
game map); the 74 cutscenes reachable by a jump play with no fault.

**The player's report, 25 Sep (morning), on the 07:24 JSRF.app:**

> The disappearing police seems to have disappeared. … Performance seems
> better. There seem to be some occasional slow downs. Text still has a thin
> line artifact above the text. Replaying the tutorial, boost looks a little
> strange still.

## Order

1. **G70 — shader-compile hitches** (the "occasional slow downs"). The
   player's session compiled 114 specialised fragment pipelines on first use,
   synchronously on the draw thread: 5.66 s in total, worst 883 ms; single
   frames of 1.06 s and 1.91 s mid-play and 6.3 s at start-up. Fix in
   progress: compile in the background and draw with the generic pipeline
   until ready, and persist pipelines in a Metal binary archive so later
   sessions do not compile. Done when a second session shows ~0 ms of draw
   thread compile wait and no frame over ~50 ms from compilation.
2. **G71 — the thin line above text** (G2's neighbour). Not seen by
   `clobber.py`, which counts quad positions, not pixels. Suspect bilinear
   sampling bleeding the atlas row above the glyph, or a half-texel offset.
   First step: an unattended flight recording of Corn's Garage dialogue and a
   per-draw look at the text quads; the player's M during the artifact is the
   other route.
3. **G72 — tutorial boost looks strange.** Needs a capture: the player's M
   during a boost, or the tutorial driven by the harness with the boost
   pressed. Compare with xemu. GG-Notebook's mission byte 0x14 = D8 reduces
   the boost effects, an A/B switch for which part is ours.
4. **G66 — the ADX lock leak** (mitigated, hangs gone). Origin found in
   principle: during the main thread's pass the guest issues one unlock more
   than it locks while the ADX thread is blocked holding a level. A caller
   chain trace (8fdccfb) is running to name the guest function; fix then.
5. **G2 — corrupt glyphs.** Re-measure with the Garage reproducer (the
   historical 1.7% predates the 21 Sep fence fix 5358eec, which measured
   0/177, 0/173, 0/313 after); running.
6. **Performance.** The player says it is better. Measured: most stages
   18–31 ms (60 Hz cap), Sky Dino 34 ms after G68. Hashed texture lookup
   merged (removes the bigger cache's lookup cost; A/B running). Beyond that,
   the cost is spread over vertex shading, submit and guest time: the D3D
   lift (G50–G52) is the lever.
7. **G65** one-frame camera pop (e200, e221, 4:62): xemu comparison.
   **G69** pushbuffer desync (1 of 8 Sewage runs): post-mortem armed.
   **G63** Poison Jam chase cutscenes: need the chase played.

## Confirmed by the player today

- G57: the disappearing police (and DJ K, crows) is gone.
- Performance better (G68 texture cache, plus 24 Sep's combiner
  specialisation).

## Waiting on the player's decision

- **Push.** The public repo would receive the old ChapterSelect port in
  history (a987c6c, be48155, e3a4907's edit): rewrite those out before the
  first push, or ask KeybadeBlox.
- **Regenerate** the gen tree with the G57 lifter fix; then delete
  x87_lift.c.
- **Upstream frndint PR**: `docs/upstream/PR_DRAFT_frndint_rounding_control.md`.
- **Cleanup**: merged branches and agent worktrees.
- **Still to check in game**: graffiti studio (G59), water (G60), Sky Dino.

## How to work

`docs/jsrf/TOOLS.md`. Reach the scene unattended before theorising; press M
(or `RECOMP_FLIGHT_AT`) to record it; one game instance at a time, and pause
unattended runs while the player plays.

## Progress, 25 Sep (midday)

- **G70 shader hitches: fixed** (167bc1e): pipelines compile off the draw
  thread and persist in a Metal binary archive. Rokkaku-dai cold session: draw
  thread waited 7.7 ms total (worst 6.3 ms) for compilation, against 5.66 s
  (worst 883 ms) in the player's morning session; warm session 205 archive
  hits, 0 misses. No CPU/GPU vertex-split flicker seen (glitch watch).
- **G66 leak: fixed at the root** (9074ea0): CRI's outer nesting word
  (0x25EFEC, sub_0013C460/C480) was read and written outside the lock; both
  bodies now run under the ADX guard. 8/8 runs of 4:96 with no dump of any kind.
- **G2 re-measure:** 0 clobbers in 813 opportunities (4 Garage runs); the
  21 Sep fence fix holds. G71 (the thin line above text) is separate.
- **Texture hash:** within noise in game (2:40 19.6 vs 19.9 ms); kept on.
- **G57 regenerated** and the run-time lift removed; the gen alone gives 0
  transients on the DJ K intro.
- **Decisions carried out:** history rewritten (the ChapterSelect port's
  blobs replaced by the clean-room file in all unpushed commits; tip tree
  unchanged) and pushed to origin main; upstream PR #126 opened; 7 worktrees
  and 15 branches removed (branches with unique commits kept).
