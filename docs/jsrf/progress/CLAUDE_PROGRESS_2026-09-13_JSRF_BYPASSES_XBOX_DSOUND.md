# JSRF never enters our DirectSound layer

13 Sep 2026. Structural, durable, and it invalidates a whole class of
comparison against the sibling projects.

## The finding

`src/audio/` (the `xbox_dsound` target) is **dead code for JSRF**.

* `xbox_DirectSoundCreate` — the layer's entry point, `dsound_device.c:379` —
  has **no callers anywhere** in `src/`, `diagnostics/` or `tools/`. The only
  other mentions in the tree are two comments in `xbox_memory_layout.c` and a
  symbol-mapping unit test.
* JSRF links Microsoft's DSOUND.lib **into the XBE**. It is section 2 of the
  image: `[ 2] DSOUND VA=0x0019E340 vsize=116060 rsize=115500`, and
  `tools/symbols/test_map_names.py` carries `_DirectSoundCreate@8 ... DSOUND.lib`
  as its fixture. So the title's DirectSound is *guest code*, recompiled to C
  with the rest of the game.
* That recompiled DirectSound drives the MCPX registers directly through the
  MMIO trap. One 154 s run: `[MCPX-TRAP] faults=164744 apu=14677 vp=14522`.

## Why it matters

The open audio defect is a ~300 ms silence at a music track change, and the
voice-event stamps put it exactly between `off68` and `on68` -- the guest
stopping and restarting its own music voice. That decision is made by
**Microsoft's DirectSound, executing as recompiled guest code**. Nothing in our
HLE audio layer participates.

So comparing our `xbox_dsound` against the sibling projects cannot help:

* **burnout3-research** takes the OPPOSITE path. Its `dsound_device.c` is
  explicitly stubbed ("actual audio playback is deferred to a later phase",
  "Streams are more complex - stub for now") and its SFX are routed through a
  separate software mixer. It exercises the HLE layer JSRF never touches, and
  does not exercise the VP/voice path JSRF lives in.
* **hl2-recomp** and **bloodwake**, the other original-Xbox titles in the
  family, have no `src/apu` at all -- their `src/` is `game` + `loader`.
* **xboxdashboard-research** has no audio of any kind.

## What this rules in

The only reference that can say whether a 300 ms gap at that track change is
correct is **xemu running the same guest code**, because the code making the
decision is the game's, not ours. That makes scene-accurate xemu automation the
blocking task for this defect, not any further source comparison.

See also: [[jsrf-audio-is-gated-not-starved]] for the measurements, and the
13 Sep handover for the resampler fix that closed the other half of this.
