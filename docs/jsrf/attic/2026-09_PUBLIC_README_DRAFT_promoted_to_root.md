<!-- DRAFT front page for the public repository.
     Promote to /README.md at publication; kept here so the root stays
     upstream's and `git merge upstream/main` keeps working until then. -->

# Jet Set Radio Future — static recompilation

A static recompiler that translates the original Xbox build of **Jet Set Radio
Future (US)** to C, and runs it natively on macOS (Apple Silicon) against a
replacement Xbox kernel, an NV2A graphics model and an MCPX APU model.

It boots, renders, plays music, takes a controller, and is playable.
It is **not finished** — see [STATUS.md](docs/jsrf/STATUS.md) for exactly what
works and what does not, with the measurements behind each claim.

![status](https://img.shields.io/badge/state-playable%2C%20not%20finished-orange)

## This repository contains no game data

No ROM, no ISO, no XBE, no assets, and none have ever been committed — the
history has been checked. You supply your own dump of a game you own. The
recompiler reads it locally; the generated C is hundreds of megabytes and is
never committed either.

## Requirements

* macOS on Apple Silicon (developed on an M1 Max). A Windows build exists and is
  used as a differential oracle, but macOS is the primary host.
* CMake, a C toolchain, and a Python 3 that has `capstone` installed.
* Your own dump of Jet Set Radio Future (US), as `default.xbe` plus its media.

> `regenerate.sh` runs whatever `python3` resolves to unless you set `PYTHON`.
> If `capstone` is installed for the system interpreter but not the one first on
> your `PATH` (a common outcome with Homebrew Python), the translator dies at
> import. Check with `python3 -c 'import capstone'` and pass `PYTHON=` if needed.

## Build and run

```sh
# 1. translate the guest XBE to C (hundreds of MB, gitignored)
PYTHON=/usr/bin/python3 diagnostics/jsrf_first_fault/regenerate.sh

# 2. build
cmake -S diagnostics/jsrf_first_fault -B <build> -DRECOMP_GEN_DIR=<gen dir>
cmake --build <build> -j

# 3. play  (JSRF_GAME_DIR points at the directory holding your default.xbe)
JSRF_GAME_DIR=/path/to/your/dump diagnostics/jsrf_first_fault/play.sh
```

There are ~100 opt-in `RECOMP_*` switches for diagnostics. Enumerate them rather
than guessing:

```sh
grep -rhoE 'RECOMP_[A-Z0-9_]+' src diagnostics | sort -u
```

## How it is built

```
tools/      the translator pipeline (disasm -> function identification -> lift to C)
src/        the runtime: kernel replacement, NV2A model, MCPX APU model, input
diagnostics/jsrf_first_fault/   the JSRF harness, tests and scripts
docs/       technical notes and measured status
```

The translator emits one C function per guest function, with guest registers as
thread-local globals and guest memory mapped at its original virtual addresses.
The HLE line sits at the `xboxkrnl` import boundary: Direct3D, DirectSound and
XAPI are the title's own statically-linked code, recompiled and driving the
hardware models at register level.

## Credits and licensing

Derived from [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp),
MIT licensed — see [LICENSE](LICENSE).

The MCPX APU and parts of the NV2A model are extracted from
[xemu](https://github.com/xemu-project/xemu) and remain **LGPL-2.1-or-later**,
with their original copyright intact. Every such file is listed in
[NOTICE](NOTICE); the licence text is in `LICENSES/`. xemu is also the reference
this project measures itself against, and a great deal of what is known about
the NV2A and MCPX is owed to that project, to Cxbx-Reloaded, and to xboxdevwiki.

## Contributing

The most useful contribution is a measurement. This project has repeatedly been
misled by confident reasoning about what the guest "must" be doing — the
handovers in `docs/jsrf/` are a long record of hypotheses that measurement
killed. If you change behaviour, say what you ran and what it printed.

One known blind spot, stated plainly: every scripted input schedule here parks
the player, so nothing skates, nothing grinds and no sound effect ever finishes.
Two real defects hid behind that for the life of the project. A pad schedule
that actually plays the game would be worth more than most patches.
