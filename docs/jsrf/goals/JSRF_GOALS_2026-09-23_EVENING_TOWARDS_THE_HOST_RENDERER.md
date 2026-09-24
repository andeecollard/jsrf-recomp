# JSRF goals — towards the host renderer, 23 September 2026 (evening)

> Superseded for ordering, 23 September 2026 (night), by
> `JSRF_GOALS_2026-09-23_NIGHT_AFTER_THE_DSOUND_LIFT.md`. Kept for the record; the active list is
> the newest goals file in this folder.

Supersedes `JSRF_GOALS_2026-09-23_THE_D3D8_BOUNDARY.md` for ORDERING. That file
keeps the record of G32–G39: the boundary census, the calling conventions, the
Clear lift, hardware texture sampling, exact early-Z, the ADX blocking-wait fix,
and every G39 slice with its numbers. Nothing there is retracted.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

## Where the lift stands

From D3D's own calls and state alone, the host knows each draw's:
- textures (4 known mismatches, one object);
- render target and depth buffer;
- viewport and scissor;
- blend, alpha, depth and stencil state;
- vertex constants;
- programmable vertex and pixel shaders;
- texture filter, wrap and LOD bias;
- and, for every fixed-function 3D draw, its transform (381,234/381,234).

Every one of these is checked draw by draw against the NV2A executor, with a
positive control. The measured shape of the title:
- **70% of draws use the fixed-function vertex path.** Mode 4, or mode 6 for
  pre-transformed geometry.
- **88% use fixed-function combiners.**

Cxbx-Reloaded (`../Cxbx-Reloaded`, read-only, GPL, reference only) confirms the
XDK enums found here. It implements the fixed-function pipeline as host
shaders over D3D state, which is the model to follow.

## Lift track

### G40 — textures from the device, not from the SetTexture hook

XbSymbolDatabase's device-offset dump names `m_Textures` at device `+0xA78`.
Read the bound texture per stage from there at each draw, instead of mirroring
`SetTexture`.

**Done when** the texture check runs from the device array and the 4 address
mismatches either vanish, or are explained by address and path.

### G41 — vertex streams and index data

Hook `SetStreamSource(stream, pVB, stride)` and `SetIndices(pIB, base)`.
- **Streams.** At each draw, every enabled NV2A vertex array
  (`SET_VERTEX_DATA_ARRAY_OFFSET`/`FORMAT`, 0x1720/0x1760) must lie inside a
  bound stream's buffer, with that stream's stride.
- **Indices.** For `DrawIndexedVertices`, the first indices D3D was handed
  must equal those the executor drew.

**Done when** both are checked on a tutorial run with a positive control.

### G42 — fixed-function lighting, material, texture transforms and fog

Mirror `SetLight`/`LightEnable`/`SetMaterial`, the texture transforms (states
2–5) with `TEXTURETRANSFORMFLAGS`/`TEXCOORDINDEX`, and the fog render states.
Establish their NV2A counterparts the way G39's eighth slice did: a discovery
pass, then a checked rule. Use Cxbx's `FixedFunctionState` and
`FixedFunctionVertexShader.hlsl` as the reference for semantics.

**Done when** each has a rule checked against the executor with a control,
or a recorded reason why its NV2A form cannot be compared.

### G43 — fixed-function combiners

These cover 88% of draws. Texture-stage words 12–21 (COLOROP/ARG0–2,
ALPHAOP/ARG0–2, RESULTARG) plus TFACTOR become NV2A combiner registers in
D3D's 0x197F90.

Two routes:
- **(a)** model the result host-side, as Cxbx's
  `FixedFunctionPixelShader.hlsl` does, and compare pixels;
- **(b)** transcribe 0x197F90 and compare registers.

Start with (b)'s discovery: the distinct (stage words -> combiner registers)
pairings in a real run. Their count decides which route is cheaper.

**Done when** every fixed-function draw's combiner registers, or its host
equivalent, follow from D3D state, with a control.

### G44 — the first frame region drawn by the host

With G40–G43 in place, draw one class of draws with a Metal path fed from D3D
state, not from the executor. Candidates:
- the pre-transformed 2D draws (mode 6);
- a programmable-shader class, whose state is already fully checked.

Compare against the executor per draw, using the Phase 3 differential mode
in the spec (`../plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_D3D8_BOUNDARY.md`),
and then by image.

**Done when** that class renders through the host path with the picture
matching and 0 faults.

## Player track

### G36 (carried) — hardware texture sampling becomes the default

`RECOMP_METAL_HW_TEX=1` is in the player's `paths.conf`. Two sessions ran on
it: no texture complaints, 10.2 M hardware samples, 0 decode failures.
**Next:** flip the default in `nv2a_metal.m`, in a commit that quotes those
sessions, after one more player check of the police and DJ K flicker with
the switch off, to rule it out.

### G45 — the police-chase freeze (USB)

See `../progress/PROGRESS_2026-09-23_THE_POLICE_CHASE_FREEZE_IS_USB.md`.
During heavy rumble, one OHCI TD error. The guest's XPP DPC (`sub_001C29F7`)
then loops forever with interrupts masked, on the main thread.
- Log the erroring TD: ED, direction, condition code and buffers.
- Read the DPC's loop exit.
- Reproduce with rumble and a pad attached.

**Done when** the police chase plays through with rumble.

### G46 — the audio the player hears

Garbled intro music, police audio garbled, DJ K audio truncated, speed and
wobble changes. The ADX guard's blocking-wait release (`b563762`) is in, but
has not yet fired in a session.

**Done when** each report is tied to a counter or a trace, one at a time.

## Order

1. **G45.** It ends sessions, and the player cannot test anything past it.
2. **G40, G41.** Small, and they close the per-draw state set.
3. **G43**, discovery first, because it covers 88% of draws. Then **G42**.
4. **G44.**
5. **G36 and G46** alongside, as sessions allow.

## Added late on 23 September

### G45 — status

`21fcbc3` leaves a half-built control TD queued instead of stalling it, and
retries the control list from the periodic tick. The player's session showed
one such TD: a rumble SET_REPORT SETUP read with CBP 0 and NextTD 0 behind a
TailP already past it. `tds_incomplete` on the `[OHCI-WDH]` line counts them.
**Still owed:** a police chase played through with rumble.

### G47 — the DSOUND crash is DISPATCH_LEVEL that only one thread honours

The same session crashed at t≈523 s in `sub_001A2E2E`, which sits in
DSOUND's APU driver between `CMcpxAPU_ServiceDeferredCommandsLow` (0x1A21ED)
and `CMcpxVoiceClient_SetFilter` (0x1A332D). It walked a voice list whose
owner pointer read 0, so the list head's forward link was NULL and the record
pointer came out as −0x4C (EAX=FFFFFFB4). The main thread was inside
DrawIndexedVertices when it ran, so this was DSOUND's DPC delivered on the
main thread.

**The mechanism, measured statically.** DSOUND protects its voice lists
with the XDK's scoped IRQL guard at 0x1A1B7C/0x1A1BAF: raise to
DISPATCH_LEVEL if below it, lower on exit. 56 call sites take it. On the
Xbox, one CPU makes that a process-wide exclusion of every DPC. In
`kernel_hal.c`, IRQL is a thread-local, and the bridge's DPC delivery checks
only the delivering thread. A DSOUND DPC can therefore run on one host thread
while another is halfway through a list edit at "DISPATCH_LEVEL". The one
crash is consistent with that, but no run has yet caught the overlap.

**Short-term fix (G47a).** Model the uniprocessor: a process-wide recursive
dispatch lock, taken when a thread raises to DISPATCH_LEVEL or above and by
every DPC and ISR delivery, released when IRQL drops. Count contention.
First instrument it (count DPC deliveries while another thread holds
DISPATCH_LEVEL) so the fix has a measured before.

**The real fix (G47b): lift DSOUND at its boundary, as for D3D.**
XbSymbolDatabase finds 137 DSOUND symbols in JSRF at XDK 4134
(`experiments/dsound_boundary/xbsymbol_dsound_4134.tsv`):
`DirectSoundCreate`, `CreateSoundBuffer`, `IDirectSoundBuffer_Play/Stop/
SetFrequency/SetVolume/SetBufferData/Lock/SetLoopRegion/GetCurrentPosition`,
the 3D setters, `DirectSoundDoWork`. Cxbx-Reloaded
(`src/core/hle/DSOUND`) replaces exactly these with host audio and never runs
the title's APU driver. Doing the same here removes the MCPX voice model,
the DSOUND DPC, this race, and the ADX guard's reason to exist. Every audio
report in G46 lives downstream of that model. Spec it the way the D3D lift
was specced: census the call sites first, then mirror, then replace.

### G48 — the DSOUND lift (started 23 Sep, at the player's request)

Spec: `../plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_DSOUND_BOUNDARY.md`. It
supersedes G47a: with DSOUND replaced, its DPC no longer runs.
- **Phase 1, census: done.**
  - Static: 193 game call sites into 55 DSOUND entry points, all stdcall,
    none reached through data.
  - Runtime: 194,156 calls through the wrappers, 0 ABI mismatches, 0 faults.
- **Phase 2, done:** the host model (`src/apu/dsound_host.c`) and
  `dsound_host_test`.
- **Phase 3, first run done:** shadow mode. It accepted 384 of 384 buffers.
  88% of cursors agree within 10 ms. The GetStatus disagreement is DSOUND
  never clearing PLAYING after Stop, which spins CRI's `mwSndStop`; the
  23 Sep black-screen hang sat in that loop. See the plan's shadow section.
- **Phase 4, built:** `stage_dsound_census.py --lift`, host bodies in
  `dsound_lift.c`. A silenced tutorial run on it:
  - 0 faults, 0 ABI mismatches over 179,679 calls;
  - 384 of 384 buffers, 0 bad handles, the effects image understood
    (20 effects);
  - `[APU-VOICE] on=0`: the APU path never ran.
  JSRF.app is built from it, with `RECOMP_DSOUND_LIFT=1` in the player's
  `paths.conf` (backup `paths.conf.bak-20260923-dsound-lift`).
- **Not yet modelled:** DSP/reverb, 3D, filters, envelopes, LFO. Windows
  output.
- **Owed:** the player's session — tutorial audio, the police chase, the cop
  fight without the hang.

**Done when** the tutorial and the police chase play on the lift with
correct audio, and the player confirms the G46 reports are gone.
