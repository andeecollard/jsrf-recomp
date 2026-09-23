# JSRF goals — lift as much as we can, 23 September 2026 (late)

Supersedes `JSRF_GOALS_2026-09-23_NIGHT_AFTER_THE_DSOUND_LIFT.md` for
ORDERING. Every earlier goals file keeps its record.

**The player's direction, 23 Sep 2026:**

> Developing the lifters has been a massive benefit today. … Set goals lets
> lift as much as we can!

**Why lifting.** Upstream xboxrecomp's design lifts each Xbox library at its
API: D3D8 to Direct3D 11, DSOUND to a mixer, XPP to XInput. It keeps xemu's
NV2A and APU models only as a fallback. JSRF took the emulation route for
graphics and input, because that is what first got it on screen.

The DSOUND lift (G48) showed what the other route buys:
- the intro garble was found against the source file and fixed;
- the cop-fight hang ended;
- a whole class of APU timing defects left the path.

**Where the frame goes.** The player's session, 23 Sep, 1,005 s:
- **Rate:** median 50 fps, p10 42, p90 60.
- **Heavy scenes, 20.5 ms a frame:**
  - vertex shading 4.9 ms;
  - submit 5.0 ms;
  - GPU wait 6.5 ms;
  - rest 4.0 ms.
- **The title's own recompiled code** is inside "rest" and costs 3–4 ms.

The time is the emulation layer, not the recompiler. A D3D lift draws from
cached native state, so it removes most of vertex shading and submit, and the
destination-read slow path from the GPU.

## Status at the start

| library | today | goal |
|---|---|---|
| DSOUND | **lifted** (G48), default in the player's app | phase 5 cleanup (G54) |
| XPP / XInput | emulated: OHCI USB model + XID pad | **lift (G49)** |
| D3D8 | per-draw state mirrored and checked; drawing still by the NV2A executor | **lift (G50–G52)** |
| kernel | host code | — |
| CRI ADX / Sofdec | recompiled title code, now fed properly (f3a9b33) | — |
| MMATRIX, XGRAPHC | small pure libraries, recompiled | census only (G55) |

## G49 — lift input at XInput

XbSymbolDatabase names the whole boundary:

| function | address |
|---|---|
| XInitDevices | 0x1BCBCC |
| XGetDevices | 0x1BD5FF |
| XGetDeviceChanges | 0x1BD621 |
| XInputOpen | 0x1C3BA1 |
| XInputClose | 0x1C3C16 |
| XInputGetCapabilities | 0x1C3C22 |
| XInputGetState | 0x1C3E14 |
| XInputSetState | 0x1C3E85 |
| XInputPoll | 0x1C3EBD |

Also `MU_Init` for memory units.

1. **Census.** Same tools as DSOUND: `census.py`/`abi.py` style static
   census, plus a runtime wrapper count and ABI check.
2. **Host bodies.** They answer from the host gamepad backend the USB pad model
   already reads (`xbox_SetUsbPadHook`). Rumble goes to the existing rumble
   hook. Device arrival and removal are reported through XGetDeviceChanges.
3. **Switch.** `RECOMP_XINPUT_LIFT=1`, carried in every build through the same
   configure-time overlay as the DSOUND lift.

**Done when** the tutorial and the police chase play on it with rumble, with
the OHCI model idle (no TDs retired). Expected side effects:
- the USB freeze class (G45) leaves the path;
- possibly Roboy's graffiti studio, which does not paint today.

## G54 — DSOUND lift, phase 5

1. **Stream cursor lead.** Re-measure `RECOMP_DSOUND_STREAM_LEAD_MS` 0 against
   100, now that CRI gets 59.5 passes/s. Drop it to 0 if underruns stay 0,
   since it adds 100 ms of latency.
2. **Default on.** Make `RECOMP_DSOUND_LIFT` default-on in code, quoting the
   player's sessions of 23 Sep. Take it out of `paths.conf`.
3. **ADX guard.** Check the ADX guard's role on the lift, and retire it if its
   lock never contends.
4. **Old models.** Stop starting the APU model when the lift is on. Keep it
   for one more week as the switch-off fallback.

## G53 — the frame rate we lost since 16 Sep

The 16 Sep handover measured 16.4–17.0 ms in gameplay; the tutorial now reads
20.4 ms. Cheap to find, and it stacks with the lift.
- **A/B each suspect on an idle host:** HW_TEX, METAL_FF, TEXMODE_APPROX,
  WILD_PTR(_SELFTEST), ACTMAN_REPORT, IRQ_LATENCY, KERNEL_THREADS and the rest
  of `paths.conf`'s diagnostics. Score each by `[STAGE]` and `[FRAME-WIN]`.
- **Keep:** only diagnostics that are needed for a player session.
- **Profiler:** Apple's (`xctrace`) aborts on this macOS build (rc 134, 23 Sep).
  Use our counters: `RECOMP_METAL_CB_GPU`, `RECOMP_METAL_CB_STATS`, `[STAGE]`.

## G50 — D3D lift, part 1: the per-draw state, complete

Close what the host cannot yet derive. These are carried from G40–G43, with
the combiners moved up because they gate drawing:
1. **G40:** textures from `m_Textures` (+0xA78); explain the 4 mismatches.
2. **G43:** fixed-function combiners (88% of draws). Discovery: the distinct
   (stage words → combiner registers) pairings.
3. **G41:** vertex streams and indices.
4. **G42:** fixed-function lighting, material, texture transforms, fog.

**Done when** each is checked against the executor with a positive control.

## G51 — D3D lift, part 2: the host draws

The host draws a class of draws from D3D state with its own Metal pipelines,
and the executor skips those draws. The classes, in order:
1. **Pre-transformed 2D (mode 6)** — HUD, text, fades. It includes the boost
   and cutscene overlays the player sees flicker.
2. **Programmable-shader draws.** Their state is already fully checked.
3. **Fixed-function 3D**, after G50.

For each class:
- a per-draw differential against the executor (Phase 3 of the D3D spec);
- then images scored against the executor's;
- then a player session.

**Done when** all three classes draw on the host with the picture matching.

## G52 — D3D lift, part 3: the executor leaves the frame

- The host presents its own render target, with no read-back.
- The pushbuffer executor runs only for what is not yet lifted, then for
  nothing.
- **Target:** p99 frame time ≤ 16.7 ms in the player's heavy scenes.

## G55 — census of what is left

After G49 and G51, list every library call that still reaches emulated
hardware (NV2A, MCPX, OHCI, SMBus, EEPROM). Each one is either lifted or has a
written reason to stay.

## Carried: the player's graphics reports

These are to be checked as the classes move to the host (G51):
- boost flicker;
- the police and DJ Professor K cutscene flicker;
- the Poison Jam cutscene missing elements;
- Rokkaku-dai Heights drawing no buildings (screenshot of 23 Sep: sky, ground
  colour, HUD, dialogue, one graffiti arrow).

**Rule for each:** capture it first on the executor, so the lift is judged
against a known-bad frame and not from memory.

## Order

1. **G49**, input lift. Small, and it removes a whole emulated stack.
2. **G54.1 and G53**, cheap and unattended.
3. **G50 → G51.1** (mode-6 2D). The first host-drawn pixels, and the flicker
   the player sees most.
4. **G51.2, G51.3, G52.**
5. **G54.2–G54.4 and G55** alongside, as sessions confirm.

## Progress, 23 Sep (late)

- **G49: built, awaiting the player** (`5249f6e`).
  - The input lift reaches the tutorial with the USB model idle (0 TDs), 0
    faults, and 0 ABI mismatches over 208,949 lifted calls.
  - Four title dependencies were found and matched to XPP's own code: the
    packed capabilities struct, the shared device-type object, insertion
    after enumeration latency, and a handle that is a real device record.
  - `RECOMP_XINPUT_LIFT=1` is in the player's `paths.conf` (backup
    `paths.conf.bak-20260923-xinput-lift`).
  - Owed: rumble in play, and a controller unplugged and replugged.
- **G54.1: done.** On the intro, the stream lead measures 0 underruns at 0 ms
  now that CRI gets 59.5 passes/s. The default is back to 0, which removes
  100 ms of audio latency.
