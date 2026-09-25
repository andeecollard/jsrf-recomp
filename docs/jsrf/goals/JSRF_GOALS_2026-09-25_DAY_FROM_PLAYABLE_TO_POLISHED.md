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

## Player session on the 10:36 JSRF.app (25 Sep, late morning)

> 1. works 2. water present, boost working properly!

- **G59 confirmed**: the graffiti studio paints.
- **G60 confirmed**: the Rokkaku-dai water is present.
- **G72 closed**: the tutorial boost looks right (no longer strange; most
  likely G57 or G60 — not bisected).
- **G70 confirmed in play**: draw thread waited 28.4 ms in total for pipeline
  compiles (worst 18.1 ms) against 5.66 s (worst 883 ms) this morning; worst
  frame 189 ms against 6.3 s; mean 17.7 ms. Log
  `~/jsrf-build/jsrf-first-fault/measure/player-2026-09-25-1036build.log`.

**Open, in order:** G71 thin line above text; G65 one-frame camera pop
(xemu); performance beyond 60 fps in heavy stages (the D3D lift); G63 Poison
Jam chase cutscenes (needs the chase); G69 pushbuffer desync (post-mortem
armed).

## G71 — the thin line above text: probably authentic (25 Sep, midday)

A 50 % bilinear blend of the atlas row above the glyph cell. In Corn's
"This is the GG's Garage. / Hey, where's our pizza?" only the four 's' glyphs
show it: a ~9 px grey dash on the quad's top pixel row. The cell above 's' is
'g', whose descender reaches the cell's last row (alpha 4/15) in the SHIPPED
font (the atlas page in guest RAM is byte-for-byte jetfont.dat at 1048832).
The text vertex program's 0.53125 bias, truncated to 1/16, puts the quad's top
edge at exactly a pixel centre; the top-left rule includes the row, and v there
is exactly the cell boundary, so bilinear weights the two rows 50/50 (measured
≈0.54). Hardware coverage (abaire's nxdk_pgraph_tests goldens,
Vertex_shader_rounding) and xemu (trunc(pos*16)/16, GL bilinear, passes those
tests) make the same choices; filter, wrap, texel centre and DXT3 decode all
check out. So our pixels should match xemu and the console. **Not changed.**
To close: an xemu capture of that line, zoomed above each 's' (x 143–151 and
174–182, y 60 at 640×480). An opt-in "clamp bilinear to the glyph cell" switch
is possible as a documented enhancement, not a correctness fix. Evidence in the
25 Sep session scratchpad (G71-*.png, run-before, run-glyph, cap1).

## The D3D lift (G50–G52), 25 Sep afternoon

Pushed to main at e560be4 (793635e, 568ca7d, e560be4); defaults unchanged.
The host arm is `RECOMP_D3D8_HOST_2D=draw RECOMP_D3D8_HOST_FF=draw
RECOMP_D3D8_HOST_VS=draw RECOMP_D3D8_HOST_FF_GPU=1`. It draws 97–98.5% of
rasterised batches. Stage modes and the PS final combiner agree with the
executor on every draw checked; VS draws match; FF keeps 1–4 px edge
differences. Frame means, FLIP_PACE=0, map_run free play:

| stage | executor | host arm |
|---|---|---|
| Garage 1:00 | 17.70 | 16.22 |
| Rokkaku-dai 2:40 | 19.36 | 15.25 |
| Shibuya Terminal 2:10 | 27.17 | 24.78 |
| Sky Dino 6:60 | 33.57 | 26.91 |

Player baseline, same afternoon, executor only (the switches were not yet in
paths.conf): mean 18.3 ms, p50 17.0, p90 22.5, 121 frames over 33 ms in 27k
(`measure/player-2026-09-25-lift.log`).

Before a default change: host pipelines in the binary archive (e9f097d, done:
warm session 70 hits, 0 compiled, worst build 0.3 ms), no GPU drain on the host's first bind each frame (~2 ms), a player
session with the arm on. Then host draw encode (~4 ms/frame in Sky Dino).

### Evening: the "visual bugs" were an experiment, not the lift

The player watched harness windows (Corn's hat "knocked out", "a lot of
visual bugs"). Those were step-2 arms with an uncommitted
`RECOMP_METAL_ASYNC_FLIP`: the flip stopped writing the bound surface back to
guest RAM, and the window showed stale, half-drawn frames in both arms. Parked
on branch `parked/step2-async-flip`, not in any build. The pushed host arm,
compared whole-frame with the executor (`gametools/frame_match.py`), differs
only by motion and 1-LSB rounding in five scenes; Corn's hat is solid in both.

Step 2 (bind drain): `RECOMP_METAL_DEFER_SWAP` removes the bind drain
(16.6 s -> 5.8 ms over 8.2k binds) but the flip drain grows by the same
amount: Rokkaku 15.25 -> 14.69 ms, Sky Dino unchanged. Something unaccounted
absorbs the saved time; find it before any presentation work.

Player sessions of 25 Sep (15:53, 17:36) ran WITHOUT the lift: paths.conf
has no `RECOMP_D3D8_HOST_*` lines, and the log says `[D3D8-MIRROR] off`.
Check that line before calling a session a lift session.
