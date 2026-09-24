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
- **G53: closed, the premise was wrong.** No regression is shown.
  - The "16.4–17.0 ms on 16 Sep" was a different scene from the tutorial's
    20.4 ms.
  - In the harness's own mission scene (scene=30, `ab_switch.sh`), the
    idle-host A/B of 23 Sep 15:45 (`measure/idlehwtex_ab_20260923-154534`)
    reads 17.0 ms with HW_TEX: 59 fps, at the line.
  - What stands between us and 60 is heavy scenes: the player's p10 is
    42 fps. There, `sync` (GPU time, 7–9 ms) is the largest stage, then
    submit and vertex shading. That is G51/G52's target, not a switch to
    turn off.
- **Metal refusals in the player's 1,005 s session: none** — 0 texture
  formats refused, 0 rejected draws, 0 software fallbacks. So the cutscene
  flicker and Poison Jam's missing elements are draws made wrongly or at the
  wrong moment, not draws dropped.
- **G40: done, with one mismatch recorded, not yet explained.**
  - The mirror now reads each stage's texture from the device's m_Textures
    (+0xA78). Over a 150 s tutorial it agrees with the SetTexture hook on
    2,400,000 of 2,400,000 stage-draws, so the device is the source.
  - The 4 remaining address mismatches are one object, 0x0436B6C0, a 64x64
    DXT1. D3D reads Data 0xF2F000; the GPU sampled 0xF2E000, 4 KB lower.
  - It is not emission timing: latching at the first draw after a binding
    change, and latching at SetTexture itself, both left all 4 in place.
  - The format word's bit 0 is set (DMA context B). The comparison ignores
    context bases, so a context base offset is the next suspect. The lift
    must reproduce whichever it is.
- **"Floaty" gameplay, measured (24 Sep): the title is frame-stepped.**
  - CActMan's anim counter (+0x87E0) advances exactly at the frame rate:
    48.8/s at 48.8 fps in the tutorial (`[ACTMAN-TIME]`,
    `RECOMP_ACTMAN_REPORT=1`).
  - No catch-up step exists, and save-game timers are kept in frames
    (`CSaveData::SetTimer(eTIMER, dwFrames)`).
  - So game speed = fps / 59.94: the tutorial runs at 81%, and the player's
    median 50 fps is 84%. Only a steady 60 fps fixes it, which makes G51/G52
    the gameplay fix as well as the frame-rate one.
- **Agents, 24 Sep:** G43's static transcription of D3D's fixed-function
  combiner builder, and G41 (streams and indices in the mirror), each in its
  own worktree. Neither runs the game; the lead runs their builds.
- **G54.2: done (24 Sep).** `RECOMP_DSOUND_LIFT` is default ON in code;
  `=0` is the APU path.
- **Upstream:** PR #121 (ADPCM reserved byte and index clamp, with
  tests/adpcm_decode) opened. The KeSetEvent and OHCI fixes were checked
  and do not port.

## G56 — the lifter's unresolved flags (found 24 Sep, reviewing upstream PRs)

JSRF's generated C carries **326 `_flags` fallbacks** -- the lifter's "UNRESOLVED
FLAGS, branch never taken" path, reading a variable nothing assigns:
- **~200** are `sete` into edx/eax;
- **many** are `jp`/`jnp` after FPU compares (MSVC's fnstsw/test ah/jp idiom for
  float comparisons), so the comparison always goes one way;
- **at least one** is `loop`, the case upstream PR #110
  (NoRain211, open) fixes. Upstream PR #120 (open) fixes narrow result-sign
  tests.

Any of these can silently break game logic -- culling, physics, collision --
and so they are a suspect for "buildings missing" at Rokkaku-dai Heights.

1. **Census.** Each site's address, instruction, function, and whether that
   function runs (func-hit trace).
2. **Lifter fixes.** Take #110 and #120 into our lifter (MIT, same code base).
   Resolve parity after `fnstsw`/`sahf`, and the `sete` producers.
3. **Regenerate on a copied gen tree.** `regenerate.sh` overwrites in place; see
   the memory note. Then A/B the tutorial and a replay.

Also from upstream's open PRs, already equivalent in our tree: #102
(primitive numbering) and #104 (rcl/rcr). Moot on our path: #118 (USB
GET_REPORT, under the XInput lift) and #103/#108 (the software sampler and
rasteriser; JSRF draws on Metal). Worth a look: #119 (KeQuerySystemTime
resolution).

**G56, first sample (24 Sep).** The first site, in `sub_0012D120`, is a `jp`
at `loc_00100058`, the first instruction after an entry the recompiler split
off. The flags it tests were set by code in the preceding function, and the
lifter does not carry flags across a function boundary. So part of G56 is
function splitting (flags live across a split point), not missing instruction
support. The census must classify each site as "producer in this function"
or "producer across a boundary"; the fixes differ: lifter semantics versus
merging or carrying flags at split entries.

## Progress, 24 Sep

- **G41: done** (agent, merged `5a64476`). The mirror derives each draw's
  vertex arrays from D3D's stream table (0x19DCE8, 16 x {stride, offset,
  VB}) and the vertex-shader object's attribute records, and its indices
  from SetIndices (device+0x38C/+0x1C, 0x19DED4).

  | tutorial, 120 s | draws | arrays exact | indices match | hooks agree |
  |---|---|---|---|---|
  | normal | 479,980 | 1,772,326 / 1,772,326 | 479,980 / 479,980 | 479,980 / 479,980 |
  | positive control | 499,960 | 0 / 1,847,829 | 0 | — |

  So every draw's textures, surfaces, viewport, state, constants, shaders,
  transforms, vertex arrays and indices now follow from D3D alone. What is
  left is the fixed-function combiner and lighting state (G43 runtime half,
  G42).
- **G43 static half: done** (agent, merged).
  - `src/nv2a/d3d8_ff_combiner.c` transcribes D3D's fixed-function
    combiner builder 0x197F90, SetRenderState_TextureFactor 0x18ECC0 and the
    final-combiner tail of 0x195610, with 96 hand-derived checks
    (`jsrf_d3d8_ff_combiner`).
  - Notes: `experiments/d3d8_boundary/ff_combiner_notes.md`.
  - Render-state indices in 4134 are 10 lower than Cxbx's 5933 numbering
    (SPECULARENABLE 93, POINTSPRITEENABLE 108, TEXTUREFACTOR 129).
- **G43: done** (agents, merged). With the player's switches, over a 120 s
  tutorial, every fixed-function draw's 51 combiner registers (CONTROL,
  COLOR/ALPHA ICW/OCW x8, FACTOR0/1 x8, SPECULAR_FOG_CW0/1) follow from D3D
  state through the host transcription.

  | run | fixed-function draws | all registers match | distinct setups |
  |---|---|---|---|
  | normal | 478,622 | 478,622 | 13 |
  | control | 459,543 | 0 | 13 |

  - D3D's lazy rebuild (dirty bit 0x800, flusher 0x1964A0) never disagreed
    with draw-time state.
  - **Only 13 distinct combiner setups** cover 88% of draws: the host
    renderer needs 13 pipeline variants for them.
- **The destination read is not the GPU cost** (24 Sep, player switches,
  tutorial). `RECOMP_METAL_SHADER_BLEND=0` read sync 10.68 ms and 56.2 fps;
  mode 3 read 10.59 ms and 56.6 fps. The 16 Sep correctness fix is free. The
  ~10.5 ms of GPU time per frame is still unexplained.
- **Profile, tutorial (24 Sep `sample`).**
  - CRI's counting spinner `sub_0013B180` burns a core.
  - The main thread spins in D3D_BlockOnTime `sub_00191440`.
  - `getenv` sat on hot paths; fixed in `68e9724`.
- **Harness runs default to NOT the player's configuration.** `measure.sh`
  does not read `paths.conf`, so HW_TEX and METAL_FF were off: 48.7 fps
  against 57.1 fps with them. Source `paths.conf` (without the game/HDD paths)
  when a number is meant to describe the player's build.
- **G56: resolved, with one live bug found** (agent, merged `ae6d32b`; census in
  `experiments/lifter_flags/census.md`).
  - 279 of the 326 `(_flags` reads are real: rep compares, `lock xadd`,
    `cmpxchg`. The ~200 `sete` are correct IID compares.
  - The 47 unresolved sites are all dead:
    - 17 are in `tail_jump_alias` copies with no static caller (this includes
      0x00100058, the sample);
    - 29 are jump tables disassembled as code;
    - 1 follows a `ret`.
  - Fixed:
    - upstream #110 (LOOP) ported;
    - #120's test taken; the lifter half was already in our tree;
    - rep compares now set CF and keep ZF on a zero count. That was a live
      bug in `std::string::compare` (sub_00179AE0): every mismatch returned
      +1, and "" compared wrongly.
  - pytest: 635 passed, 2 pre-existing failures (test_mem_watch,
    test_bridge_stack_args).
  - Regeneration on 24 Sep (backup `gen-2026-09-24-PRE-G56-KEEP`) applies it:
    285 lines change.
- **CRI idle spinner lifted** (`9f18d66`): process CPU 312% -> 221%.
- **Regenerated, 24 Sep 01:18** (git_head 9066487; backup
  `gen-2026-09-24-PRE-G56-KEEP`).
  - 285 lines changed, as predicted.
  - Tutorial with the player's switches: 0 faults, scene 61, 56.3 fps.
  - JSRF.app rebuilt from it.
  - `jsrf_apu_list_cycle_off` failed once under `ctest -j8` and passed 4 of 4
    alone: a parallel-run flake, not a regression.

## The frame rate, 24 Sep (late): 60 at correct speed in the tutorial

- **Combiner specialisation** (agent, `e2aee7b`, merged). Fragment pipelines are
  specialised per combiner program with Metal function constants. Tutorial,
  player's switches, 2 trials per arm, idle host:

  | arm | GPU wait | frame | fps |
  |---|---|---|---|
  | generic | 9.9 ms | 17.0-17.2 ms | 58.2-58.9 |
  | specialised | 2.6 ms | 11.5-11.6 ms | 86.0-86.9 |

  45 pipelines, 8 ms of compiling in all (worst 0.4 ms).
- **...which exposed that flips were never paced.** The game is frame-stepped,
  so 87 fps meant 145% game speed (anim 87 ticks/s). Light scenes had already
  reached 110-147 fps in the player's session. Flips are now held to one
  vblank period (`e4acb21`, `RECOMP_FLIP_PACE`):
  - tutorial 59.8 fps, anim 59.8/s, GPU wait 1.06 ms;
  - intro music matches `title.adx` in every second once playing;
  - the logo sequence now takes its real-time length (it had run fast).
- The Windows cross-build compiles again (`nv2a_metal_frag_force_arm` guarded).
- **G42: done** (agent, merged). Tutorial with the player's switches, exact
  matches throughout:

  | group | registers | exact | control |
  |---|---|---|---|
  | texgen | 3,871,776 | all | 0 draws |
  | texture transforms | 1,605,568 | all | 0 |
  | lighting | 1,299,042 | all | 0 |
  | fog | 999,936 | all | 0 |

  - 338 lit draws, all one directional light. Texgen NORMAL_MAP runs on
    stages 0 and 1 (the cel-shading lookup). Texture-transform layouts are
    A and E only. Fog is never on in the tutorial.
  - All laziness cross-checks read 0, and the guest's rsqrt constants were
    confirmed live.
  - **Every per-draw input the host renderer needs now follows from D3D**,
    except the inverse model-view (0x580), which needs 0x190A30 transcribed.
    G50 is complete; G51 (host draws) is next.
- **G42b: done** (agent, merged). The inverse model-view (0x580, guest
  0x190A30, a cofactor inverse in x87 with D3D's approximate rsqrt) is
  transcribed bit-exact: 0 mismatched words over 3,000,000 random matrices
  against the recompiled guest.
  - In the game (tutorial, player's switches): 23,223 draws needing it,
    278,676 of 278,676 registers exact; control 0; laziness 0; singular 0.
  - **Every per-draw input the host renderer needs now follows from D3D
    alone, with nothing left unchecked** in the tutorial.
- **G51.1, first in-game comparison** (agent, `b3c0f15`; registration fix
  `56d526d`). The host draws the pre-transformed 2D class from D3D state in
  shadow, against the executor's own depth (read-only peek). Tutorial,
  player's switches:
  - 15,773 draws compared: 11,696 EXACT, 156 within tolerance, 3,921
    mismatching. Pixels over tolerance: 10.0M of 342.7M.
  - Control: 324.5M over tolerance, so the comparison bites.
  - All 2D draws are LEQUAL with depth write on.
  - Likely main cause: "index data changed before the token" = 14,699. The
    host read indices from the dynamic index buffer after the game had reused
    it; the executor draws D3D's pushbuffer copy. With the agent.

## Progress, 24 Sep (early morning)

- **G54.4 done** (`0a74ad2`). With the lift on, the APU model is not started
  (`RECOMP_APU_MODEL=1` forces it). Every lift run had read guest_methods=0 and
  frames total=0, and the model still opened a second audio device in the
  player's build. Tutorial, 110 s: 0 faults, anim 59.8/s, lift counters
  identical to the previous run. JSRF.app rebuilt with it.
- **G54.3 not done, on purpose.** The ADX guard serialises CRI's
  priority-elevation lock (sub_0013B0A0/0E0). That is a single-CPU guarantee
  and has nothing to do with the APU model's timing, so the lift does not
  retire it. The premise in §G54 was wrong.
- **G51.1, the 2D shadow:**
  - Indices from the draw-time snapshot (`7b6a1a6`): vertex bytes stable,
    12,974 of 12,974 index lists taken from the call.
  - The half-pixel bias (`43b1dee`): D3D's pass-through adds 0.53125, the
    NV2A pixel-centre bias, which the executor applies through c-37 =
    (320.531, 240.531). The logo draw went from max r20 g40 b20 on 11,543 px
    to max r2 g3 b2 on 2,058 px. Over-tolerance pixels fell from 71.6M to
    25.3M.
  - Open: EXACT collapsed from 4,430 to 604, and depth mismatches rose to
    14,028 of 14,032. The c0/c1 self-check reads the wrong slots (it prints
    0s). The agent is on these.
- **G51.1 shadow: 13,980 of 13,981 exact** (`c39afeb`, run h2d-shadow5, tutorial,
  player config). The executor truncates screen x/y toward zero to 1/16 px
  (`prepare_vertices`, and the VSH epilogue), so D3D's k + 0.53125 lands on
  k + 0.5. The host now snaps the same way. That single rule explains the EXACT
  collapse, the depth failures (every full-screen quad's first row plus first
  column: 640 + 480 − 1 = 1,119 px) and the logo's residue. The self-check
  now reads the bias from c-37 − c-38: 13,981 as assumed, 0 different.
  - Over-tolerance pixels: 3,152 (was 25.3M). Depth: 1 draw mismatching.
  - The one left: flip 2078, a 306-vertex HUD text draw (fvf 1C4, 256x256
    tex 0E) where the host covered 6,818 px and the executor 3,682.
