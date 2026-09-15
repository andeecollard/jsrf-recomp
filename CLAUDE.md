# CLAUDE.md

Orientation for a session starting cold. Durable facts only — anything that
changes run to run belongs in `docs/jsrf/`, not here.

## What this is

A static recompiler for original-Xbox titles, derived from
[`sp00nznet/xboxrecomp`](https://github.com/sp00nznet/xboxrecomp) and
specialised to bring up **Jet Set Radio Future (US)** on macOS ARM64.

`origin` is a standalone private repo — *not* a GitHub fork. `upstream` is a
plain remote we fetch from and merge, currently a few hundred commits behind us.

The recompiler translates the guest XBE to C, which is compiled against a
replacement Xbox kernel, an NV2A graphics model and an MCPX APU model in
`src/`.

## Layout

```
src/                        the runtime: kernel, apu, gpu, memory layout
tools/                      the translator pipeline (disasm, func_id, recomp)
diagnostics/jsrf_first_fault/   the JSRF harness — main.c, tests, backports
docs/jsrf/                  all JSRF research notes (see its README)
  handovers/  progress/  goals/  plans/
```

Everything at the repo root is upstream's. Keep it that way: new JSRF notes go
in `docs/jsrf/`, so `git merge upstream/main` stays clean.

## Build and run

The generated C is **not** in git — it is hundreds of MB, rebuilt by
`diagnostics/jsrf_first_fault/regenerate.sh`, and lands in `build-macos/`
(gitignored).

```sh
cmake -S diagnostics/jsrf_first_fault -B <build> -DRECOMP_GEN_DIR=<gen dir>
cmake --build <build> -j
ctest --test-dir <build>            # 28/30; the two failures are
                                   # deliberate gates, see CMakeLists.txt
```

```sh
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_REPORT_MS=10000 RECOMP_HDD_ROOT=<disposable emulated-hdd copy> \
  <build>/jsrf_first_fault
```

The build needs nothing outside the repository. It used to: `recomp_manual.c`
came from a sibling `jsrf_stock_test` checkout via `JSRF_STOCK_DIR`, which
meant a fresh clone failed in CMake and every worktree needed a symlink beside
it. The file is vendored at `diagnostics/jsrf_first_fault/recomp_manual.c`.

Running is different: the game dump and the emulated HDD tree to copy from
cannot be vendored, so they stay outside. Every script in
`diagnostics/jsrf_first_fault/` that stages an HDD reads `JSRF_HDD_SRC` for
the latter and names it when the tree is missing. The default is still a
sibling checkout, which is nobody else's layout — set the variable.

There are ~100 `RECOMP_*` opt-in switches. Enumerate them rather than guessing:

```sh
grep -rhoE 'RECOMP_[A-Z0-9_]+' src diagnostics | sort -u
```

Prefer running against a **verified archived gen** over a fresh regeneration
when reproducing a past result — regeneration is not bit-stable across
translator changes.

## Where the title has got to

Boots, renders, presents, reaches gameplay, and produces audio. Known open
items live in the newest file in `docs/jsrf/handovers/`; read that before
starting, and trust it over anything older, including older handovers that
describe the same subsystem.

Handovers are dated and **supersede each other**. An older one describing a bug
as unexplained does not mean it is still unexplained.

## Rules learned the hard way

These were each paid for with a wrong conclusion. They are the reason the
handovers read the way they do.

**Measure, don't infer.** Do not assert runtime state from static reading of
generated C. If a claim is about what the guest *does*, it needs a run behind
it.

**Read a counter's trigger before trusting its value.** Several counters here
have lied — one differenced against the wrong timestamp and read ~0; upstream's
`sleep_acc_us` has the same flaw. A counter is evidence only once you have read
what increments it.

**Every absence-measurement needs a positive control.** `on=0` means "no voice
started" *or* "the instrument is dead". Pair it with a nearby counter already
known to move, or the zero proves nothing.

**A guarded MMIO page loses writes if anything else unprotects it.** The APU
aperture is trapped read-only so guest stores reach the model. A thread that
unprotect/store/reprotects *anything* on that 16 KB page opens a window where
guest stores complete silently against RAM — this cost 13.5M windows in 45 s and
swallowed every `VOICE_ON`. Emulate the access in the handler; never open a
writable window. `PCRTC_INTR_0` and `PGRAPH_INTR` still have this shape.

**Don't patch around a gate until a run proves which value is wrong.** Forcing a
count nonzero or clearing a flag bit hides the producer bug that set it.

**Known-bad reasoning shortcuts**, all of which have misled before: `PUSHER
draws=0`, the D3D11 sink's method list, absent `USB-PAD` lines, and comparing
log sizes across scenes.

## Conventions

- Commit messages here are prose explaining *why*, in the style of the existing
  log. Match it.
- Instrumentation is **opt-in and read-only** (`RECOMP_VOICE_TRACE`,
  `RECOMP_PAD_SENTINEL`, …) and installed into a *copy* of a gen tree by a
  script in `diagnostics/jsrf_first_fault/`, never committed into a fix.
- Preserved baselines and their archives are read-only. Verify against
  `RESTORE_MANIFEST.txt` before use; extract only into scratch.
