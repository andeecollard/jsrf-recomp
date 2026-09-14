# Jet Set Radio Future — static recompilation

Turning the original Xbox release of **Jet Set Radio Future** into a native
macOS binary by recompiling its x86 code to C, rather than emulating it.

Built on [**sp00nznet/xboxrecomp**](https://github.com/sp00nznet/xboxrecomp)
by sp00nz (MIT). The recompiler, the Xbox kernel replacement, the NV2A graphics
model and the MCPX audio model all come from that project; this repository is a
fork specialised to get one title running, with the general fixes sent back
upstream where they belong.

> **This is a work in progress, not a release.** It boots, renders, plays music
> and reaches gameplay. It also runs at about half the frame rate it should,
> crashes in roughly one run in five, and gets the intro card transitions
> wrong. The honest state is in [docs/jsrf/STATUS.md](docs/jsrf/STATUS.md) and
> it is kept current with measurements rather than impressions.

## You need your own copy of the game

**No game data is contained in this repository, and none will be.** Not the
executable, not the assets, not the generated C — that last one is mechanically
derived from the game's own code and is rebuilt locally by `regenerate.sh`,
never committed. You supply a dump of a disc you own; the repository supplies
the machinery that runs it.

## Build

```sh
cmake -S diagnostics/jsrf_first_fault -B build -DRECOMP_GEN_DIR=<gen dir>
cmake --build build -j
ctest --test-dir build
```

```sh
diagnostics/jsrf_first_fault/play.sh
```

`JSRF_GAME_DIR` points at the directory holding `default.xbe`. There are around
a hundred `RECOMP_*` switches for instrumentation; enumerate them rather than
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
  have lied, including two found lying this month.
- **Every absence-measurement needs a positive control.** `on=0` means "nothing
  happened" or "the instrument is dead", and only a control separates them.

If you are looking for somewhere to start, the open items at the bottom of
[STATUS.md](docs/jsrf/STATUS.md) are real and individually tractable.

## Licence and credit

MIT, inherited from xboxrecomp — see [LICENSE](LICENSE), © 2026 sp00nz.
Upstream's own README is preserved at
[docs/upstream/README.xboxrecomp.md](docs/upstream/README.xboxrecomp.md), and
the [sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu) is where the
wider project happens.

Jet Set Radio Future is © SEGA. This project is not affiliated with or endorsed
by SEGA, and distributes none of their material.
