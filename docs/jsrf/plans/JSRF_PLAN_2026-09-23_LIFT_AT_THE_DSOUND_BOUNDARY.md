# Lift JSRF's audio at the DSOUND boundary — 23 September 2026

The audio counterpart of `JSRF_PLAN_2026-09-23_LIFT_AT_THE_D3D8_BOUNDARY.md`.
Instead of running the title's own DSOUND library against our model of the
MCPX APU, replace DSOUND's public entry points with host code that mixes the
title's sounds and plays them through macOS. The APU model, DSOUND's
interrupt, DPC and timer, and the voice lists they fight over all stop
running.

## Why

Every open audio report sits downstream of the APU model:
- garbled intro music, police audio, DJ K truncated, speed and wobble (G46);
- the DSOUND crash of 23 Sep (G47): its DPC walked a voice list another host
  thread was editing, because our IRQL is per thread and the Xbox's is per CPU;
- the black-screen hang after the cop fight on 23 Sep, where a game thread sat
  in `IDirectSoundBuffer_GetStatus` while the game drew nothing (unproven);
- the ADX guard, which exists only to paper over the APU model's timing.

Cxbx-Reloaded does exactly this lift (`src/core/hle/DSOUND`), locating the
functions with XbSymbolDatabase. Here the recompiler already gives every
function a C symbol, so no patching is needed: a wrapper takes the name.

## The boundary, measured

### Statically (`experiments/dsound_boundary/census.py`)

XbSymbolDatabase names 137 DSOUND symbols in JSRF at XDK 4134
(`xbsymbol_dsound_4134.tsv`). DSOUND is 410 functions at 0x19E340–0x1A786F.

| | |
|---|---|
| call sites from game code into DSOUND | 193 |
| distinct DSOUND targets | 55 |
| game functions that call DSOUND | 49 |
| targets reached only through data (vtables, callbacks) | 0 |

The callers are two libraries:
- **CRI ADX** (`CRI::mwSnd*`, around 0x141400–0x141B00): music and voice.
- **Smilebit's sound driver** (0x168000–0x172000): effects, 3D and the
  listener.

`abi.py` records each target's calling convention in `entry_points.json`.
Every one is stdcall, pops a fixed byte count, and is called with a
consistent argument count at every site.

Four targets have no XbSymbolDatabase name:
- 0x19E4BC and 0x19E4C1 are stubs that return 0;
- 0x19E523 is DSOUND's internal work pump;
- 0x1A24A3 is a scalar-deleting destructor.

### At run time (`stage_dsound_census.py`, `RECOMP_DSOUND_CENSUS=1`)

The census ran for 150 s through the tutorial, silenced: 194,156 calls
checked, **0 ABI mismatches**, 0 guest faults. Game calls in that run:

| entry point | calls | what it is for |
|---|---:|---|
| IDirectSound_SetOrientation / SetPosition / SetVelocity | 46,200 each | listener, every frame |
| IDirectSoundBuffer_GetStatus | 18,306 | polling "still playing?" |
| IDirectSound_CommitDeferredSettings, DirectSoundDoWork | ~7,800 each | per frame |
| IDirectSoundBuffer_Lock / Unlock | 6,766 each | ADX streaming |
| IDirectSoundBuffer_GetCurrentPosition | 3,576 | ADX pacing |
| CreateSoundBuffer / SetBufferData / SetLoopRegion / SetCurrentPosition | ~384 each | effect bank load |
| Stop / Release | 651 / 354 | effect bank unload |
| Play | 8 (+4 inside DSOUND) | matches `[APU-VOICE] on=12` |

That run triggered few sounds, so Play is low. Real sessions reach hundreds
of voice starts.

**What the sound data looks like:**
- **Effects** are Xbox ADPCM (format tag 0x69), 22,050 Hz, mono (block 36)
  or stereo (block 72). The title owns the memory and hands it over with
  `SetBufferData` (`bytes=0` in the descriptor). Loop region and position
  are set once at load.
- **Music and voice (ADX)** play through four 64 KB PCM buffers,
  16-bit stereo at 44.1 kHz. DSOUND owns their memory. CRI fills them with
  `Lock`/`Unlock`, paced by `GetCurrentPosition`, and plays them looping.
- **DSP effects.** `DownloadEffectsImage` runs once. `SetEffectData` and
  `GetEffectData` have 50 static sites but were not called in the tutorial.

## Design

A host DSOUND in `src/apu/dsound_host.c`, pure C and unit-tested without the
title:

- **Objects.** `DirectSoundCreate` and `CreateSoundBuffer` return small
  zeroed guest allocations as handles. Guest code never reads inside them
  (no data references from game code; confirm at run time). A host table maps
  each handle to its state. `AddRef`/`Release` keep the COM refcount.
- **Buffer memory.** `SetBufferData` records a guest pointer. A
  DSOUND-owned buffer (`bytes>0`) gets guest memory, because `Lock` hands
  the title a pointer it writes through.
- **Voices.** Each buffer has:
  - state: playing, looping;
  - play cursor, in bytes of source data;
  - loop region;
  - frequency, volume (hundredths of a dB), headroom;
  - 3D: position, distances and mode.
- **Mixer.** Runs on the audio device's thread at 48 kHz stereo. It decodes
  PCM16 or Xbox ADPCM with the tested `adpcm_decode_block`, resamples by
  `frequency / 48000`, and applies volume and a simple 3D attenuation and pan
  from the listener. I3DL2 reverb and the DSP image are later.
- **Queries.** `GetStatus` and `GetCurrentPosition` answer from the mixer's
  cursor. The write cursor is the play cursor plus a fixed lead. These two are
  what the title's control flow depends on: CRI paces on the one, and the
  effect driver retires voices on the other.
- **The rest.**
  - `DoWork` and `CommitDeferredSettings` apply deferred 3D;
  - effect-data calls keep a copy, so a `Get` returns the last `Set`;
  - the capability and speaker-config calls return fixed values.
- **Threading.** One mutex over the host table and voice state, held briefly
  by callers and the mixer. No guest code ever runs on the mixer thread.

## Phases

1. **Census.** Done; the tables above.
2. **Host model and its tests.** The mixer, ADPCM, loop regions, cursors and
   status, checked against synthetic buffers with known answers.
3. **Shadow mode** (`RECOMP_DSOUND_LIFT=shadow`). The original DSOUND still
   runs and still drives the APU. The wrappers also feed the host model,
   which renders to a WAV file instead of the speakers. Check that:
   - every buffer the title creates is one the model understands (format,
     size, ownership);
   - the model's `GetStatus` agrees with DSOUND's on every call (the play
     cursor is timing-dependent, so compare it within a tolerance);
   - the WAV sounds right.
4. **Replace** (`RECOMP_DSOUND_LIFT=1`). Host bodies replace the originals,
   `DirectSoundCreate` never initialises the APU, and the ADX guard is off.
   Done when the tutorial and the police chase play with correct audio and
   0 faults, and the player confirms the four G46 reports are gone.
5. **Remove** the APU model from the default path, once the player has run
   sessions on the lift.

## Risks

- **Hidden internal state.** CRI or the effect driver could depend on a
  DSOUND side effect the census cannot see, such as a field read through a
  returned pointer. Shadow mode's status comparison exists to catch this.
- **Timing.** The title paces ADX on `GetCurrentPosition`. A host cursor
  that advances smoothly is closer to hardware than the APU model's frame
  steps, but it must never run backwards or jump.
- **3D and reverb.** The Xbox HRTF and I3DL2 will not match at first.
  Positional volume and pan get most of the way; the rest is Phase 5+.

## Shadow results, 23 Sep (phase 3, first run)

`stage_dsound_census.py --shadow`, 150 s silenced tutorial, 0 guest faults:
- **Buffers.** All 384 buffers the title created were accepted by the model;
  0 refused.
- **Cursors.** Of 4,235 GetCurrentPosition calls, 3,723 (88%) agree within
  10 ms. 252 differ by 100 ms or more; not yet explained.
- **GetStatus.** 2 of 19,382 calls agree. Every disagreement is DSOUND
  saying PLAYING where the model says stopped, and every one is the same
  pattern. CRI's `mwSndStop` (Stop at 0x141672, GetStatus at
  0x14168E/0x1416D4) stops an ADX stream buffer, then polls GetStatus until
  it reports stopped. On the APU model, DSOUND goes on reporting PLAYING long
  after Stop, and the CRI thread spins. The host model answers "stopped" at
  once, as a stop is meant to.
- **The 23 Sep hang.** The black-screen hang after the cop fight has this
  shape. `sample` of the hung process showed a game thread in
  `sub_00141640 -> IDirectSoundBuffer_GetStatus`, inside this same loop. With
  the lift, that loop ends on its first poll.
- **The WAV.** It holds real audio from the moment the ADX music starts
  (t≈15 s): RMS 1,900–5,900, peak under 30,100, no clipping. It needs a listen.
