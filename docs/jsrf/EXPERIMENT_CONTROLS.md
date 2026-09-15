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
from `$JSRF_HDD_SRC` — which defaulted, and still defaults, to
`../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd` — and nothing
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

## RECOMP_USB reaches nothing (14 Sep 2026)

Seven scripts here set `RECOMP_USB=1`. The only `getenv("RECOMP_USB")` in the
tree is at `src/usb/ohci.c:805`, and that file is not wired to anything: none
of `xbox_OhciInit`, `xbox_OhciOwnsAddress`, `xbox_OhciHandleMmio` or
`xbox_OhciReport` has a single reference outside itself. The library is still
linked, so it compiles and does nothing.

The live USB model is `src/kernel/xbox_usb_ohci.c`, reached through the MCPX
write trap, and it needs no switch.

This matters beyond tidiness: a run that set `RECOMP_USB=1` and saw no USB
activity from `ohci.c`'s counters was reading an unwired file, and a handover
records that exact reasoning voiding an elimination of two interrupt theories
on 11 Sep. **Any conclusion resting on that file's counters is void.**

The switch is removed from the scripts. `src/usb/ohci.c` is left in place
because it is upstream's, and deleting it would diverge the tree for no gain.

## Two things a green `ctest` does not cover (14 Sep 2026)

**`tests/` at the repository root is inert.** `tests/audio_mixer`,
`tests/d3d8_smoke`, `tests/mmio_decode` and `tests/xaudio2` are built by
nothing: no `add_subdirectory(tests)` exists in any CMakeLists, and two of the
four have no CMakeLists of their own. They are upstream's and are left in
place, but **nobody should read 28/28 from `ctest` as covering them** -- the
28 are `diagnostics/jsrf_first_fault`'s, and that is the whole of the C-side
coverage this fork runs.

**The APU asserts are a latent footgun, not a live bug.** `src/apu/apu_vp.c`
has 22 `assert()` calls, several on values the guest supplies directly --
`assert(current_voice < MCPX_HW_MAX_VOICES)` where `current_voice` is
`d->regs[NV_PAPU_FECV]`, a register the title writes. `RelWithDebInfo`
deliberately omits `-DNDEBUG` (and that is the right call: the model is xemu's
and is held together by those asserts, so deleting them silently would be a
semantic change wearing an optimisation's clothes). The consequence is that a
malformed guest write would abort the process rather than being rejected.

Measured before changing anything: **zero aborts across 397 recorded runs** --
no `SIGABRT`, no `Abort trap`, no `Assertion failed`. So the title does not
write an out-of-range voice index, and the asserts have never fired. They are
left exactly as they are.

What this buys is a diagnosis for free later: if a crash ever presents as
SIGABRT rather than SIGSEGV, it is almost certainly one of these, and the
guest wrote something the model does not model. Look here first.
