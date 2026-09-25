# Investigating JSRF: the toolkit

How to get evidence out of the running game. Most of this exists so that a
question about a scene can be answered in a minute, unattended, instead of by
asking someone to play to it. Every switch below is read from the environment
(or from `paths.conf` for `JSRF.app`) and is off unless stated.

## Reach the scene

| tool | what it does |
|---|---|
| `RECOMP_CHAPTER_JUMP=<chapter>[:<mission>]` | From the Garage save: once the current mission has settled (`RECOMP_CHAPTER_JUMP_SETTLE` frames, default 180), end it the way the game's own mission exits do and start chapter/mission `C:M` (mission file `mssnCMM.bin`). `2:96` is the chapter-2 intro; `2:40` is Rokkaku-dai Heights. |
| `RECOMP_CHAPTER_JUMP_MARK=1` | With the jump: press the pad mark when the new mission's first event ends, so a flight recording covers the whole intro cutscene. |
| `RECOMP_CHAPTER_SELECT=<chapter>[:<mission>]` | The same destination, taken when a player picks any tutorial from Roboy's menu. |
| `gametools/catalogue/catalogue.md` | All 300 cutscene files: which mission plays each, under which flags, and the jump that reaches it. 74 play on a jump; 37 jumps cover them all. |
| `gametools/savetool.py` | Edit a save's chapter, mission, flags, events-seen and stage tags, re-encode and re-sign it (round trip is byte for byte). `--make-checkpoints` writes saves that replay specific cutscenes on load. |
| pad recordings | `JSRF.app` records every session (`padrec/*.padrec`); `RECOMP_PAD_SCRIPT=@file` replays one. Replays drift from the original route after a while: good for "what is this draw doing", not for reaching a precise moment. |

## Catch what goes wrong

| tool | what it does |
|---|---|
| `RECOMP_FLIGHT_FRAMES=N` + **M** | Keeps the last N presented frames and a one-line record of every draw in each. Pressing M in the game window (the pad mark) writes them to `RECOMP_FLIGHT_DIR` as `flight-K/frame-NNNN.bmp` + `draws-NNNN.txt`. `RECOMP_FLIGHT_AT=<guest frame>` triggers the same write without a keyboard. |
| `gametools/frame_match.py <run A> <run B>` | Pairs every presented frame of one run with the closest frame of another and scores the pixels that differ. Use it to A/B a renderer change against the executor arm on the same scene: motion scores ~2-15%, a broken picture ~80% (25 Sep: it caught a frozen-frame bug that VERIFY and the glitch watch both passed). |
| `flight_diff.py <flight dir>` | Names the draw states that come and go between neighbouring frames, and the largest picture changes. |
| `RECOMP_FLIGHT_XF_DRAW` | Adds the full vertex-program constant file per draw to the flight record (how the G57 bone-matrix blow-up was found). |
| `RECOMP_D3D8_HOST_VERIFY=N` + `_VERIFY_AFTER=F` | In the lift's draw mode, 1 flip in N is drawn by the executor and every host draw is compared with it. `_AFTER` keeps verify flips off until flip F: a verify flip at the title lost the stage harness's START in six arms of six (G74). With `RECOMP_D3D8_HOST_2D_DUMP=<dir>` the first point/line draws are dumped as pre / executor / host / difference whether they match or not. |
| `RECOMP_D3D8_HOST_POINTS=mark` | The host draws every point 12 px wide: where the title puts its points, in a presented frame. |
| `RECOMP_GLITCH_WATCH=1` | Detects frames that differ from both neighbours while the neighbours match (one-frame dropouts and short runs) live, and writes only those frames and their draws. |
| `RECOMP_MARK_BUMP=1`, `RECOMP_MARK_BUMP_ENV=1` | Paint bump-mapped draws magenta (approximated) or cyan (displaced). A general pattern: paint one class of draw a solid colour to learn whether it is rasterised where you expect. |
| `RECOMP_FRAG_FORCE=<mode>` | Replace every fragment by its texture 0 sample (1), vertex colour (2), white (3) or texture coordinate (4). Whole-frame: it shows the last full-screen draw, so use it with care. |
| `RECOMP_COMBINER_TRACE=1` + `RECOMP_DRAW_CAPTURE=<prefix>` | Capture the full register state and every transformed vertex of the first draw of each distinct combiner configuration. |

**Presentation lag.** The presented frame is snapshotted at the swap's
FLIP_STALL, before the swap's copy quad runs, so `frame-N.bmp` shows the scene
drawn in `draws-(N-2).txt`. Pair frames and draw lists with that offset.

## Read the game's own state

| tool | what it does |
|---|---|
| `RECOMP_STAGE_MEM=va:size[,va:size]` | At every stage-harness capture, dump those guest memory ranges to `mem-<capture>-<va>.bin`. |
| `gametools/progress.py` | Decode a save file or a memory dump of the save object (0x1EFFB0): chapter, mission, flags, souls, per-stage tag state, events seen. |
| `gametools/missions.py`, `evdump.py` | Mission bytecode and event (`.dat`) file parsers: what a mission plays and why, and what an event puts on screen. |
| `[FF-LIT]`, `[TEXFMT]`, `[DROP]`, `[FOG]`, `[COMBINER]` log lines | Periodic census lines in `last-run.log`. A `[DROP]` WARNING names draws that were dropped or simplified, with their texture formats. |

## Drive the game

`diagnostics/jsrf_first_fault/stage_harness/run.py` boots the engine against a
private copy of an HDD, drives it through a JSON scenario (title, load, walk,
talk), then serves a debug window (`debug.py <out> capture|press|goto|stop`).
Launchers for the common cases are in `gametools/harness/`:
`jump_run.sh` (chapter jump + captures), `replay_*.sh` (pad-recording
replays), `checkpoint_run.sh` (boot a checkpoint save). One game instance at a
time; runs silence audio with `SDL_AUDIODRIVER=no_such_driver`. The harness
occasionally fails to deliver its first input at the title; retry.

## External references

- Function and structure names: KeybadeBlox's JSRF-Decompilation symbol table
  (clone it beside the repo). It has no licence and refuses LLM-generated
  contributions: use it as a reference, do not copy from it, do not send it
  agent-written changes.
- File formats: KeybadeBlox's GG-Notebook (WTFPL).
- NV2A behaviour: xemu's `hw/xbox/nv2a` sources are the reference; xemu itself
  is the oracle when a picture is in doubt.
