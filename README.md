# Jet Set Radio Future — static recompilation

Turning the original Xbox release of **Jet Set Radio Future** into a native
macOS binary by recompiling its x86 code to C, rather than emulating it.

Built on [**sp00nznet/xboxrecomp**](https://github.com/sp00nznet/xboxrecomp)
by sp00nz (MIT). The recompiler, the Xbox kernel replacement, the NV2A graphics
model and the MCPX audio model all come from that project; this repository is a
fork specialised to get one title running, with the general fixes sent back
upstream where they belong.

> **Work in progress, not a release.** As of 26 September 2026 the game is
> playable into chapter 2 at a steady **60 fps** on Apple Silicon: story
> missions, cutscenes, graffiti, music and the controller all work, and every
> chapter can be entered. What is still wrong,
> and what has not been checked yet, is listed below and in
> [docs/jsrf/STATUS.md](docs/jsrf/STATUS.md).

## You need your own copy of the game

**No game data is contained in this repository, and none will be.** Not the
executable, not the assets, not the generated C — that last one is mechanically
derived from the game's own code and is rebuilt locally by `regenerate.sh`,
never committed. You supply a dump of a disc you own; the repository supplies
the machinery that runs it. Save files and pad recordings are yours too and
stay out of the repository.

## Where it stands

**Works** (each confirmed in a player session unless marked):

- Boot, menus, Load and character select, the Garage and the tutorial.
- Story progression through chapter 1 into chapter 2 (Shibuya Terminal,
  Dogenzaka Hill, Rokkaku-dai Heights), with tags, graffiti souls and
  characters joining; the save's progress decodes and round-trips exactly.
- Stage music streams for whole sessions on the host audio path (the DSOUND
  lift): 23-minute sessions with the track read in real time to the end.
- Rokkaku-dai's city, fog and cel-shaded characters (fixed 24 Sep: the city
  was discarded by an alpha test on a padding byte; characters were white
  because two 16-bit texture formats were never decoded on the GPU).
- Cutscenes no longer lose the whole scene for a frame once a second (fixed
  25 Sep: the recompiler's `frndint` ignored the x87 rounding mode, so the
  game's `floor()`/`ceil()` rounded to nearest and animation read past its key
  table). Verified unattended on the chapter-2 intro: 11 dropped frames → 0.
- Every chapter can be entered unattended (`RECOMP_CHAPTER_JUMP`); chapters 2
  and 5 were entered and played without a fault.

- Roboy's graffiti studio paints, and Rokkaku-dai's water is drawn with its
  bump-environment mapping (both confirmed 26 Sep).
- **60 fps.** With the Direct3D lift (below) a player session on 26 Sep ran at
  17.1 ms a frame against the 16.7 ms vsync cap, p99 19.5 ms. Uncapped, in
  free play:

  | stage | NV2A model | Direct3D lift |
  |---|---|---|
  | Garage | 18.2 ms | 8.5 ms |
  | Rokkaku-dai Heights | 21–23 ms | 8.5 ms |
  | Shibuya Terminal | 29.0 ms | 11.5 ms |
  | Sky Dinosaurs | 35–37 ms | 12.7 ms |

## The Direct3D lift

The game talks to the Xbox GPU only through Direct3D 8. Instead of decoding
the push buffer Direct3D writes and modelling the NV2A register by register,
the lift follows Direct3D's own calls and draws them on the host GPU, from
Direct3D's own derivations (texture stage modes, the pixel shader's final
combiner, point sizes, bump matrices). It now draws 99.3–99.6% of every frame;
the one draw left to the NV2A model is Direct3D's own swap-copy quad. Every
change is checked against the NV2A model per draw (`RECOMP_D3D8_HOST_VERIFY`)
and per whole frame at the same moment of a stage
(`gametools/frame_match.py`): the lift and the model differ by under 1% of
pixels, which is lighting rounding and animation phase.

It is opt-in in the engine, and on in a bundle built with
`JSRF_APP_LIFT=1 packaging/make_app.sh …`; `paths.conf` can turn any part
off. The plan and the measurements are in
[docs/jsrf/goals/JSRF_GOALS_2026-09-25_EVENING_FINISH_THE_LIFT.md](docs/jsrf/goals/JSRF_GOALS_2026-09-25_EVENING_FINISH_THE_LIFT.md).

**Still wrong or unknown:**

- One 4-second stall the first time the graffiti studio opens: a vertex
  program compiled on the draw thread (being moved off it, 26 Sep).
- A thin grey line above some speech-box letters; measured to be the shipped
  font's bilinear bleed, so probably authentic, pending an xemu comparison.
- Elements missing from the Poison Jam chase cutscenes.
- Chapters 3–9 have been entered but not played through; a sweep of all 74
  cutscenes that can be reached unattended is in progress.
- The lift is not yet the engine's default, only the app bundle's.
- Windows: the MinGW cross-build links again at the current tree (26 Sep) and
  runs under CrossOver; porting the lift's host renderer to Direct3D 11 is in
  progress. CrossOver can judge its picture, not its speed.

## Build and play

Requirements: macOS on Apple Silicon, CMake, a C toolchain, and a Python 3
with `capstone` (`/usr/bin/python3 -c 'import capstone'`; pass `PYTHON=` to
`regenerate.sh` if your first `python3` lacks it).

```sh
# 1. translate the guest XBE to C (hundreds of MB, gitignored)
PYTHON=/usr/bin/python3 diagnostics/jsrf_first_fault/regenerate.sh

# 2. build and test
cmake -S diagnostics/jsrf_first_fault -B build -DRECOMP_GEN_DIR=<gen dir>
cmake --build build -j
ctest --test-dir build

# 3. a double-clickable app with its libraries bundled (JSRF_APP_LIFT=1: the
#    Direct3D lift on, which is what reaches 60 fps)
JSRF_APP_LIFT=1 diagnostics/jsrf_first_fault/packaging/make_app.sh build/jsrf_first_fault <dest dir>
```

`JSRF.app` reads `~/Library/Application Support/JSRF/paths.conf` for the game
directory (`JSRF_GAME_DIR`, the folder holding `default.xbe`), the emulated
HDD (`JSRF_HDD_ROOT`) and any `RECOMP_*` switches, and writes its log to
`last-run.log` beside it (the previous one is kept as `last-run-previous.log`).
Do not pipe `make_app.sh` into `head`: SIGPIPE leaves a truncated bundle.

## Debugging it

The work is driven by evidence from the running game, and most of the tooling
exists to get that evidence without a person at the controller. See
[docs/jsrf/TOOLS.md](docs/jsrf/TOOLS.md) for the full guide. In short:

- **Reach any scene unattended:** `RECOMP_CHAPTER_JUMP=<chapter>:<mission>`
  starts any story mission from the Garage; a catalogue of all 300 cutscene
  files says which jump plays which (`diagnostics/jsrf_first_fault/gametools/`).
- **Catch a glitch as it happens:** `RECOMP_FLIGHT_FRAMES=N` keeps the last N
  presented frames and every draw in them, written out when you press **M**;
  `RECOMP_GLITCH_WATCH=1` finds one-frame dropouts by itself.
- **Read the game's own state:** a save decoder/editor, mission and event
  parsers, and guest memory dumps at harness captures.
- **Drive the game:** a stage harness (`diagnostics/jsrf_first_fault/stage_harness`)
  boots, navigates, replays recorded pad input and captures frames.

There are a few hundred `RECOMP_*` switches; enumerate them rather than
guessing:

```sh
grep -rhoE 'RECOMP_[A-Z0-9_]+' src diagnostics | sort -u
```

## How the work is done here

Every claim in `docs/jsrf/` is supposed to have a measurement behind it, and
several of the notes are retractions of earlier claims that did not. The rules
in [CLAUDE.md](CLAUDE.md) were each paid for with a wrong conclusion:

- **Measure, don't infer.** Runtime behaviour is not deducible from reading
  generated C.
- **Read a counter's trigger before trusting its value.** Several counters here
  have lied.
- **Every absence-measurement needs a positive control.** `on=0` means "nothing
  happened" or "the instrument is dead", and only a control separates them.
- **Reach the scene before theorising about it.** A night of guesses about
  Rokkaku-dai ended the moment the harness could get there unattended.

The current plan is the newest file in [docs/jsrf/goals/](docs/jsrf/goals/);
[docs/jsrf/README.md](docs/jsrf/README.md) explains how the notes are laid out.

## Upstream

General fixes go back to xboxrecomp as pull requests from a fork: ten have
been merged so far (lifter flag semantics, rotates, SHLD/SHRD, `movsd`
dispatch, APU mix-down, an SVOD reader). Open as of 26 Sep:

| PR | what |
|---|---|
| [#130](https://github.com/sp00nznet/xboxrecomp/pull/130) | indirect calls name their site; sites with a small recorded target set become guarded direct calls |
| [#129](https://github.com/sp00nznet/xboxrecomp/pull/129) | function detection follows a switch through a measured jump table |
| [#126](https://github.com/sp00nznet/xboxrecomp/pull/126) | `frndint` honours the x87 rounding mode (the cutscene dropouts above) |
| [#124](https://github.com/sp00nznet/xboxrecomp/pull/124) | REPE CMPS/SCAS carry flag, and ZF at a zero count |
| [#121](https://github.com/sp00nznet/xboxrecomp/pull/121) | ADPCM's reserved header byte and step-index clamp |
| [#117](https://github.com/sp00nznet/xboxrecomp/pull/117) | an untranslated instruction reports itself at run time |
| [#89](https://github.com/sp00nznet/xboxrecomp/pull/89) | bounds checks on guest buffers |

The Direct3D lift is not upstream: it is tied to this title's XDK build
(4134). The method -- follow the title's Direct3D calls, draw them on the host,
check every draw and frame against the NV2A model -- is general.

## Licence and credit

MIT, inherited from xboxrecomp — see [LICENSE](LICENSE), © 2026 sp00nz.
Upstream's own README is preserved at
[docs/upstream/README.xboxrecomp.md](docs/upstream/README.xboxrecomp.md), and
the [sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu) is where the
wider project happens.

Function and structure names for the title come from KeybadeBlox's
[JSRF-Decompilation](https://codeberg.org/KeybadeBlox/JSRF-Decompilation)
symbol table, and file formats from their
[GG-Notebook](https://codeberg.org/KeybadeBlox/GG-Notebook) (WTFPL).
[xemu](https://xemu.app) is the reference for NV2A behaviour and the oracle the
renderer is checked against.

Jet Set Radio Future is © SEGA. This project is not affiliated with or endorsed
by SEGA, and distributes none of their material.
