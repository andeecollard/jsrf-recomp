# What a run actually controls for, and what it does not

Written 14 Sep 2026, after a batch was invalidated by a provenance label that
was itself wrong. Kept short and honest: the point is the gaps, not the list.

## Controlled

| Input | How | Strength |
|---|---|---|
| Binary | one build for a whole batch | strong — verify with `shasum` |
| Generated tree | `GENERATION_MANIFEST.txt` + the `[GEN]` line in every log | strong — translator, runtime header and XBE hashes travel with the evidence |
| Probe arming | `probes=` in the manifest; `[PROBES] NOT ARMED` at runtime | strong |
| Pad input | `RECOMP_PAD_SCRIPT`, linted by `test_pad_schedule.py` | strong |
| Environment | set explicitly per arm by the batch script | strong |
| HDD writes leaking between runs | `play_scripted.sh` copies a fresh disposable tree per run | strong |

## NOT controlled, and why it matters

**The stock HDD source is checked by mtime only.** `play_scripted.sh` copies
from `../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd` and nothing
verifies its CONTENTS. As of this batch its mtime is 4 Sep 2026 and has been
stable for ten days, which is evidence but not proof: mtime does not change
under a content-preserving touch, and does not tell you whether an earlier
session's guest writes are baked into that tree. So the source is REPEATABLE
but not PRISTINE, and a batch that reproduces a result has not ruled out that
the result depends on that particular saved state.

*Next batch should:* record a content manifest of the source (a hash per file,
or one hash over a sorted listing) and verify each disposable copy starts
matching it. This batch did not, and its conclusions carry that caveat.

**Host state across a session.** CoreAudio wedges (see the memory note), the
audio device can fail to open, thermal state and other processes vary. The
`AUDIO:` verdict in `play_scripted.sh` catches the device case; the rest is
uncontrolled.

**Wall-clock scheduling.** The pad schedule is anchored at input init, but how
far the title has progressed by a given `t` varies run to run. Always read the
scene gate (`NtOpenFile` count) rather than assuming a timestamp means a scene.

## Reading rules earned the hard way

* An ISR-count plateau means **"no further ISR returns observed"**, not
  "interrupt delivery stopped". The counter increments after the handler
  returns, so a hung handler and one never entered are currently
  indistinguishable. Splitting `entered`/`returned` is queued.
* `[PAD-TRACE]` and `nonneutral=` do NOT show that a press reached the guest —
  our own injector produces them at 50 Hz. Use ordinal 175, and prefer TD
  completions for actual USB progress.
* Compare like scenes. A number from the attract screen and one from gameplay
  are not the same measurement.
