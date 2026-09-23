# JSRF goals — after the DSOUND lift, 23 September 2026 (night)

Supersedes `JSRF_GOALS_2026-09-23_EVENING_TOWARDS_THE_HOST_RENDERER.md` for
ORDERING. That file keeps the record of G40–G48, including the DSOUND lift's
phases 1–4 and their numbers; nothing there is retracted.

**Where things stand.** JSRF.app is built from the staged DSOUND-lift binary,
with `RECOMP_DSOUND_LIFT=1` in the player's `paths.conf`. The title's DSOUND
and the APU model no longer run. A silenced tutorial run on the lift:
- 0 faults;
- 0 ABI mismatches over 179,679 calls;
- 384 of 384 buffers accepted.

No one has heard it yet.

## 1. The player's session on the lift (blocked on the player)

This decides everything below it. Ask the player to play through the
tutorial, the police chase and the cop fight. Listen for:
- the intro music: is it still garbled?
- DJ K: is his audio still truncated?
- the police siren;
- anything missing, doubled, too loud or too dry.

From `last-run.log`, copied before anything else runs:
- `[DSOUND-LIFT] periodic` lines. `refused`, `bad_handle` and
  `missing_data` must stay 0, `image_bad` must be 0, and `plays` must reach
  the hundreds in gameplay.
- `tds_incomplete` on `[OHCI-WDH]`. This is G45's positive control: if it
  moved, the half-built rumble TD really arrives.
- The black screen after the cop fight must not recur. If it does, `sample`
  the process and check whether any thread is inside
  `sub_00141640 -> IDirectSoundBuffer_GetStatus`.

**If the audio is wrong,** turn the lift off by deleting the line, and fall
back to the old path while the defect is found. Shadow mode
(`stage_dsound_census.py --shadow`) then runs both paths side by side.

## 2. Unattended work that does not need the player

In this order. Each step is small and measurable.

### 2a. A WAV from the lift itself

`RECOMP_DSOUND_LIFT_WAV=<path>` should tee `dsh_mix`'s output to a file,
as the shadow does. That lets a silenced harness run be checked for
clipping, silence and dropouts, per second, by script. It also lets two
builds be compared, without ever playing audio aloud.

### 2b. Reach past the tutorial unattended

The measure pad only reaches the tutorial. Use the 6-minute replay (the G2
confirmation replay) on the lift build to reach later scenes. Check 0 faults,
no hang, and `[DSOUND-LIFT]` counters moving. If no replay reaches the cop
fight, record that; the player's session is then the only test of the hang.

### 2c. What the title asks for that the lift ignores

The argument log should cover a gameplay run for:
- `SetEG`, `SetLFO`, `SetFilter`, `SetMixBins`;
- the buffer 3D setters;
- `SetEffectData`.

Answer each question with counts:
- Does any descriptor carry `DSBCAPS_CTRL3D` (0x10)?
- Are envelopes or filters ever set to anything but their defaults?
- Which I3DL2 parameters does the title set?

Model the ones that are used, most-used first. Positional volume and pan
come first if 3D is used at all. A simple host reverb comes last.

### 2d. The cursor outliers

In shadow mode, 252 of 4,235 GetCurrentPosition answers differed from
DSOUND's by 100 ms or more. Find which buffers and moments they were; the
likely suspects are the first poll after Play or SetCurrentPosition. CRI
paces its streams on this cursor, and a jump could show up as the "speed
changes" the player reported.

### 2e. Make the lift part of the normal build

Today the lift exists only in a staged gen. Rebuilding the app from
`build-feav` silently drops it (memory note:
`jsrf-bundle-from-dsound-lift-build`). Move the wrapper generation into
the normal build, either as a `regenerate.sh` step or as a CMake custom
command over the gen, so every build carries the lift behind its switch.

### 2f. Windows output

`dsound_host_out.c` has no Windows path. Give it the SDL path (the Windows
build already carries SDL for input), or XAudio2. Otherwise the Windows build
on the lift is silent.

## 3. After the player confirms the lift (phase 5)

1. Make `RECOMP_DSOUND_LIFT` default-on in a commit that quotes the sessions.
2. Take the ADX guard out of the default path. It existed for the APU
   model's timing, and CRI now talks to the host.
3. Retire G47a, the process-wide dispatch lock. DSOUND's DPC no longer runs,
   so its race cannot occur. Keep the finding: another library that raises
   IRQL would reopen it.
4. Leave the APU model in the tree, off by default, until two more sessions
   pass. Then drop it from the player's build.

## 4. G45: the police-chase freeze (USB)

This is waiting on the same session. **Done when** the police chase plays
through with rumble. If `tds_incomplete` stays 0 over a chase, the race did
not occur that session, which neither proves the fix nor refutes it; one more
session will be needed.

## 5. The graphics lift, resumed

Unchanged from the evening file, in this order:
1. **G40:** textures from `m_Textures` (+0xA78). Explain the 4 mismatches.
2. **G41:** vertex streams and indices, checked with a control.
3. **G43:** fixed-function combiners. Discovery first: count the distinct
   stage-word-to-register pairings.
4. **G42:** lighting, material, texture transforms, fog.
5. **G44:** the first host-drawn region.

## 6. The flicker the player sees

The police, DJ K and boost all flicker, and the Poison Jam sequence has
glitches. None of this is explained yet.
1. **G36's check first.** One player session with `RECOMP_METAL_HW_TEX`
   removed. If the flicker persists, hardware texturing is cleared and its
   default can flip.
2. **Then find the draws.** A per-draw capture across a flicker frame
   (`DRAW-MIX`; see the memory note on whole-frame instruments) names the
   draw that comes and goes. Compare it with xemu as the oracle, before
   adding more instruments.

## Order

1. The player's session on the lift (§1). It unblocks §3 and §4.
2. §2a, §2b, §2c, unattended, while waiting.
3. §2d, §2e.
4. §3, once the player confirms.
5. §6.1 in a later player session. §5 continues between sessions.
6. §2f when the Windows build is next touched.

## Results of §2, 23 Sep (night)

- **§2a: done** (`34ca455`). `RECOMP_DSOUND_LIFT_WAV` and `wav_report.py`.
  A 150 s tutorial on the lift: 0 clipped samples, 1 dropout.
- **§2b: done, with a limit.** The player's hung-session recording
  (`graffiti-2026-09-23_2025.padrec`) replayed on the lift:
  - 1,226 s, 0 faults, 0 ABI mismatches over 1,282,391 calls;
  - every one-second frame sample non-black (1,219 of 1,219).

  The recording's anchors are constant, so the scene is UNVERIFIED. The run
  stayed in game sequence 58 and started only 25 sounds; it almost certainly
  drifted off the player's route. So it proves stability, not that the
  cop-fight hang is gone. That still needs the player.
- **§2c: done** (`07cd6aa`). 3D is the only ignored feature the title uses.
  - 161 of 231 buffers are 3D.
  - The listener moves every frame.
  - Reverb is always off.
  - EG, LFO, filter, effect data and mix bins are never called.

  3D distance falloff and pan are now modelled and tested. Its absence
  explains the replay WAV's 21 clipped samples.
- **§2d: done.** Nearly all of the "outliers" were the comparison ignoring
  wrap-around in the circular stream buffers. Measured wrap-aware, 2,570 of
  2,570 cursor answers agree within 20 ms (96.6% within 10 ms), with the
  model slightly ahead. The cursor does not explain the reported speed changes.
- **§2e: done** (`07cd6aa`). The `JSRF_DSOUND_LIFT` configure-time overlay;
  build-feav carries the lift.
- **§2f: code done** (`07cd6aa`). XAudio2 output compiles under mingw-w64.
  Not yet run on Windows; the overlay stays off there.
