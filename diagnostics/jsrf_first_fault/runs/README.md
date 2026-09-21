# Run recipes

One file per experiment, each naming its own question at the top and
preserving its own log and dump directory. None of them touches
`last-run.log`, so two runs never overwrite each other's evidence.

**These lived in `~/jsrf-build` until 21 September 2026 and were not under
version control at all.** That is why they are here: the snapshot instrument
they drive was worth a week of misread pictures, and the recipes that drive
it were one `rm` from gone.

They are deliberately still written against `$HOME/jsrf-build` — the built
`JSRF.app`, the preserved-logs directory and the dump directories all live
there, beside the build rather than inside the repository, and
`$SUPPORT/paths.conf` supplies the game and HDD roots. Running them from this
directory or from `~/jsrf-build` makes no difference.

## The ones that matter right now

| script | what it asks |
| --- | --- |
| `run-loadmenu-snap.sh` | **THE REPRODUCER.** Normal rendering, presented-frame captures. Player-driven: title → START → main menu → LOAD. |
| `run-loadmenu-blend.sh` | The same plus `RECOMP_BLEND_TRACE`. Counters only. Killed the multiply-blend theory. |
| `run-fragforce.sh <mode> [secs]` | Unattended, silent, any `RECOMP_FRAG_FORCE` mode. |
| `run-loadmenu-white.sh` | `FRAG_FORCE=3` with `AFTER` set. **Has never reached its own condition** — see the handover of 21 Sep night 2. |
| `run-vshcpu.sh [secs]` | The CPU-interpreter arm, the only one where the census can see `oT0`. |
| `run-t0census.sh [secs]` | The flat-texture-coordinate census. |

## The rule they exist to enforce

A picture is evidence only if the dump states its own freshness. Read the
`[SNAP]` lines first — fresh frame, `SAME FRAME AS THE LAST DUMP`, or
`NOTHING PUBLISHED` — then the images, and only then the counters.
