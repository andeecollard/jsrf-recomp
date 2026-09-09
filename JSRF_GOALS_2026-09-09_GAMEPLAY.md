# JSRF macOS — gameplay milestone review and roadmap

Date: 2026-09-09 (Europe/London)
Scope: bounded high-level review. No fixes implemented. Review only.

---

## 0. Headline correction before anything else

**There is no newer upstream xboxrecomp to port from.**

Verified live, not from cache:

```
$ git ls-remote upstream main
051a128df5ec27ef14f1ceaaead11c5457321eef  refs/heads/main

$ git merge-base --is-ancestor 051a128... HEAD  → true
```

`upstream/main` is `051a128`, and it is already an ancestor of our `HEAD`.
`git log 051a128..upstream/main` is empty. The FETCH_HEAD timestamp is today
19:35.

This matters because the review brief proposed "compare against newer upstream"
as a major workstream. That workstream does not exist in the form assumed. The
features cited — fixed-function T&L, four-stage multitexturing, register
combiners, programmable vertex shaders, unswizzling — are **already merged into
this tree**. They are in `src/d3d/` and `src/nv2a/`.

So the real question is inverted, and is a better question:

> We already have upstream's implementations. Why is JSRF's live path not
> producing correct output through them?

That reframing drives the rest of this document. Nothing below proposes porting
upstream code we already have.

---

## 1. Current verified state

Verified this session, on evidence, not inference.

| Property | State | Evidence |
|---|---|---|
| Boots on ARM64 macOS | yes | run reaches game loop |
| Logos / title / menu | yes | user-confirmed on screen |
| New Game deadlock `0x001A308E` | **cleared** | `sub_001A308E` absent from a 3 s sample; game proceeds |
| Tutorial / gameplay | begins | Shibuya Terminal, "Press the A button and jump 1 time!" |
| Audio | **silent** | user-confirmed; backend open, see §5 |
| Rendering | glitched, incomplete | user screenshot: black rect + grey bars bottom-right |
| Speed | too slow | user-reported |
| macOS window behaviour | wrong | stays behind other apps; Dock icon does not raise |
| Tutorial progression | unreliable | user-reported; **root cause unknown — see P0** |

### The build that reaches gameplay

This is not reproducible from the integration tree alone, which is why it is
archived:

| Component | Source |
|---|---|
| runtime | `xboxrecomp_upstream_integration/diagnostics/jsrf_first_fault` |
| generated code | **dirty tree's** preserved `gen` (76 dead `_flags`) |
| `sub_001A308E` | manual override in `jsrf_manual_overrides.c` |

Required launch environment — without these a run proves nothing:

```sh
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_REPORT_MS=10000 RECOMP_HDD_ROOT=<a copy>
```

Preserved as:
- git tag `jsrf-gameplay-2026-09-09` (commit `95c1d82`)
- `JSRF_BASELINE_2026-09-09_gameplay/` — binary, `gen.tar.gz`, `CMakeCache.txt`

### Measurement traps found this session

Both of these produced wrong conclusions before being caught. Record them:

- **`draws=` in the `[PUSHER]` line is always 0 by construction.** The harness
  calls `nv2a_pb_scan_set_external_executor(1)`, so the scan path never counts
  draws. Read `[GPU] N triangles rasterised` instead.
- **Without `RECOMP_PB_EXEC=1` the pushbuffer executor never runs at all**
  (`nv2a_pb_scan.c:159`). A bare launch renders nothing and says nothing.

---

## 2. Architecture summary

```
guest XBE (translated C)
  └─ guest XDK DSOUND 0x0019E340-0x001BA89B ─┐
  └─ guest D3D8 ─→ pushbuffer in guest RAM   │
                        │                     │
              nv2a_pb_scan (gated by RECOMP_PB_EXEC)
                        │                     │
              nv2a_pb_exec (CPU executor)     │
                   │            │             │
          software raster   nv2a_metal.m      │
                   │            │             │
                   └── framebuffer ──→ SDL window (d3d8_gl.c:1211)
                                              │
                                     APU MMIO trap
                                              │
                                    src/apu (xemu-derived)
                                              │
                                  apu_sdl2.c → SDL_QueueAudio
```

Threads observed live (`sample 42312`):

| Thread | Role |
|---|---|
| main | translated guest code |
| `nv2a_ack_thread` | pushbuffer acknowledgement |
| `mcpx_apu_frame_thread` | APU frame processing |
| `jsrf_adx_watch` | ADX stream watchdog |
| `PLATFORM_read_thread` | file I/O |

DirectSound note: `nm` on the shipping binary finds **no**
`xbox_DirectSoundCreate`. Upstream's `xbox_dsound` HLE is linked but never
called; JSRF runs its own statically-linked XDK DirectSound as guest code.
This confirms the prior handover and is unchanged.

---

## 3. Findings from JSRF-Decompilation

Local clone inspected directly (Codeberg blocks automated web access).

State: delinking 2.28%, decompilation 0.34% of the XBE. It is a *matching
decompilation* — a different discipline from this port, and not a source of
drop-in code.

`ghidra/objects.csv` still marks the DirectSound range
`0x0019E340-0x001BA89B` as **"DirectSound8 (need to decompose)"**. It does not
name or decompile `0x001A308E`, `0x001A2FBE`, `0x001A25CA` or `0x001A3A8E`.

**It therefore contains no answer to the current audio problem.** Its value here
is naming, for call-chain annotation:

| Address | Name |
|---|---|
| `0x000659C0` | `readInput` |
| `0x00065940` | `initInputs_MAYBE` |
| `0x00118610` | `CTrtSoundXbox::execNormal` |
| `0x00118630` | `CTrtSoundXbox::execEvent` |
| `0x00118C00` | `CTrtSoundXbox` constructor |
| `0x00168130` | `setupDirectSound_MAYBE` |

`readInput` at `0x000659C0` appeared in live samples during gameplay, which is
a useful confirmation that input is being polled in the tutorial.

Two addresses this session characterised that the decomp does **not** have, and
which are candidates to contribute back (see §11): `sub_00154D70` and
`sub_0015A020` are switch dispatchers reached only through a table, and
`0x001A2FBE` is the DSOUND completion site that clears bit 15.

---

## 4. Findings from current xboxrecomp

Already merged (§0). Assessed as present in-tree:

| Facility | Location | Present |
|---|---|---|
| register combiners | `src/d3d/d3d8_combiners.c` | yes |
| programmable VS | `src/d3d/d3d8_vsh.c` | yes |
| fixed-function T&L | `src/nv2a/nv2a_ff.c` | yes |
| texture unswizzle | `src/d3d/d3d8_swizzle.h` | yes |
| texture copy | `src/nv2a/nv2a_texture_copy.c` | yes |
| MCPX APU | `src/apu/` (xemu-derived) | yes |
| `xbox_dsound` HLE | `src/audio/dsound_device.c` | linked, **never called** |
| OHCI / USB | `src/kernel/xbox_usb_ohci.c` | yes |

Upstream's own recent history (`e3caa37`, `8146ca2`, `faa55ac`, `be52f25`)
is concentrated on **interrupt delivery, DPC queueing and vertical blank** —
i.e. runtime/scheduling behaviour, not renderer features. That is worth noting
against §6's performance findings, which are also scheduling-shaped.

Upstream's documented environment is Windows + MSVC. Our macOS/ARM64 + Metal
work is a platform port beyond their stated target.

---

## 5. Gap analysis against our implementation

### 5.1 Graphics — what JSRF demonstrably uses

From the live gameplay run's unhandled-method histogram:

| Method | Name | Count |
|---|---|---|
| `0x1800` | `NV097_ARRAY_ELEMENT16` | 916,702,775 |
| `0x1EA4` | `NV097_SET_TRANSFORM_CONSTANT_LOAD` | 8,723,634 |
| `0x0B00`–`0x0B44` | `NV097_SET_TRANSFORM_PROGRAM` | ~3.9 M each |

Also: `[GPU] ... 392 distinct` unhandled methods, ~174 M occurrences, against
a 640×480 surface at pitch 1280.

Reading, in the brief's taxonomy:

- **(A) demonstrably used:** indexed vertex submission via `ARRAY_ELEMENT16`,
  and programmable vertex shaders (`SET_TRANSFORM_PROGRAM` +
  `SET_TRANSFORM_CONSTANT_LOAD`). These are not theoretical; they are the
  highest-volume methods in the trace.
- **(B) lacking/incorrect:** at minimum the sink reporting them as unhandled.
- **(C) theoretical:** not enumerated — deliberately, per the brief.

> **The table above is the WRONG PATH. Superseded — do not act on it.** It is
> labelled *"D3D11 sink unhandled methods (CPU executor counted separately)"*,
> and the D3D11 sink is not what renders on macOS. Closed below.

### 5.1a The production path's real unhandled set (T-F closed)

`nv2a_pb_exec_report()` prints the CPU executor's own histogram as
`  [GPU]   0xNNNN xCOUNT`. It shares **nothing** with the D3D11 sink list:

| method | name | count |
|---|---|---|
| `0x0480`-`0x0498` | **`NV097_SET_MODEL_VIEW_MATRIX`** | 403,858 each |
| `0x1B00` | `NV097_SET_TEXTURE_OFFSET` | 418,559 |
| `0x1B04` | `NV097_SET_TEXTURE_FORMAT` | 418,559 |
| `0x1B4C` | texture stage 1 (stages are `0x40` apart) | 685,501 |

392 distinct methods unhandled in total.

The largest single drop is the **model-view matrix** — fixed-function T&L
transform state, uploaded as consecutive dwords from `0x0480`. Corroborating
observations: the user reports **the player character is not drawn at all** in
the tutorial, while static scenery is; an earlier project diagnostic already
identified missing fixed-function T&L as the cause of dropped scene triangles;
and the dirty tree's HEAD commit is *"The missing backgrounds are draws dropped
at the texture stage"*, which is the `0x1B00` half of this same list.

Working hypothesis for the missing character, to be tested not assumed: static
geometry survives on pre-transformed vertices, while per-object animated meshes
require `SET_MODEL_VIEW_MATRIX` to be applied; dropped, they are transformed by
stale state and land off-screen or degenerate.

**Method for the next person:** the executor prints its own list. Read
`  [GPU]   0x` lines and raise the cut with `RECOMP_PB_EXEC_TOP=<n>`. Never
reason from the `[PUSHER] D3D11 sink` line on macOS.

### 5.2 Performance — the shape is not what was assumed

Three measurements, all from the live gameplay process:

1. **The main thread is not compute-bound.** ~2664 of 3592 samples (**≈74%**)
   sit in `bridge_KeWaitForSingleObject → Sleep → nanosleep →
   __semwait_signal`. Translated guest code is *waiting*, not grinding.

2. **`nv2a_ack_thread` is a near-pure spin.**
   `[PB-ACK] 303188 loops/s: acked=325435 already=358689951` — 358 million
   "already acknowledged" iterations against 325 thousand useful acks. That is
   roughly a 1000:1 waste ratio on a dedicated thread.

3. **Diagnostic logging is a first-order cost.** The gameplay run produced
   **11 GB of log in ~15 minutes** (~44 GB/hour), with `XBOX_LOG_LEVEL=0`
   already set. `[NV2A]` lines alone were 76,654 in one 30 s window.

This substantially changes the performance story. "Too slow" was assumed to
mean rasterisation throughput. The evidence says the dominant costs are
**wait-granularity, a spinning ack thread, and I/O from instrumentation** —
all cheap to attack, and none of them requiring a Metal migration. Upstream's
own recent work on vblank/DPC/interrupt delivery (§4) points the same way.

### 5.3 Audio — where it is *not* broken

Established working, so these are excluded from suspicion:

- `[APU-SDL] output ready: 48000 Hz, 2 channel, format 0x8010`
- `[APU] started by the title (SECTL=0000000F FECTL=0000100F)` — the guest
  started the APU itself
- APU MMIO writes flowing past #1000; `[ADX] tick=…` advancing steadily
- `mcpx_apu_frame_thread` alive

So: host output is open, the guest is driving the hardware model, and the
stream decoder runs. The break is **between the APU voice mixer and
`SDL_QueueAudio`** (`apu_sdl2.c:84`), or upstream of it in voice activation.

One structural detail worth attention: `apu_vp.c:957-959` walks the voice list
and does `if (!active) break;` — a walk that **stops at the first inactive
voice** rather than skipping it. If voice activation state is wrong, that
terminates mixing early and yields silence while every surrounding counter
still looks healthy. This is a hypothesis, not a finding.

### 5.3a ROOT CAUSE OF THE SILENCE — established

Found by correlating two runs against the completion probes:

| probe site | silent run | run where the intro tune played |
|---|---|---|
| `0x001A25CA` | **0** | **65** |
| `0x001A2FBE` | 0 | 1 |
| `0x001A3A8E` | 6 | 8 |

Sound tracks whether `0x001A25CA` executes. Reading the generated code for
`sub_001A25AA` (`0x001A25AA-0x001A25F1`) gives the reason:

```
eax = MEM32(0xFE801100)      ; NV_PAPU_FECTL
eax &= 0xF00                 ; NV_PAPU_FECTL_FETRAPREASON
cmp  eax, 0xF00              ; ..._FETRAPREASON_REQUESTED
jne  loc_001A25D9            ; not requested -> skip the sound path
loc_001A25CA:                ; <- reached only when the trap IS requested
```

The live probe dump reports `+1100 guest=0000100F model=0000100F`. The trap
reason field of `0x100F` is **zero**, so the branch is taken every time and the
sound path is skipped. That is the silence.

`FETRAPREASON_REQUESTED` is set in exactly one place — `apu_vp.c:489-496`,
`case SE2FE_IDLE_VOICE`, gated on `NV_PAPU_FETFORCE1` bit 15. The dump shows
`+1504 guest=00008000 model=00008000`, so **that gate is already open**. The
missing step is therefore the call that raises it:
`fe_method(d, SE2FE_IDLE_VOICE, v)` at `apu_vp.c:1197`, which fires only when a
voice's `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` bit is found clear during the
voice-list walk.

In short: **voices never retire, so the front-end trap is never requested, so
the guest never takes its sound path.**

**Correction, and a correction of a correction.** The prior session note said
"APU voice retirement needs FE trap method `0x8000`". Earlier in this review I
declared that unsubstantiated — because I searched for `NV1BA0_PIO_*` names and
the word "retire" and found neither. That was my error. The method is named
`SE2FE_IDLE_VOICE` and `apu_regs.h:216` defines it as **`0x00008000`**. The
original note was accurate; the retraction was not. Weight the original note.

### 5.4 macOS window behaviour — root cause identified

The window is created at `src/d3d/d3d8_gl.c:1211` with flags
`SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN` only.

Across `src/` and the harness there is **no** `setActivationPolicy`, no
`activateIgnoringOtherApps`, no `makeKeyAndOrderFront`, and **no `.app` bundle
or `Info.plist` anywhere**. The binary runs as a bare Mach-O from a shell.

That is sufficient to explain all three reported symptoms: an unbundled
process has no `Info.plist`, so it gets no proper Dock identity, and nothing
ever asks the app to become frontmost. This is a packaging/lifecycle issue and
is **entirely separable** from Xbox emulation — exactly as the brief requires.

### 5.5 Static recompilation

- The integration tree's **own** freshly regenerated code reaches the game loop
  but **renders nothing**. It is not the baseline; the dirty tree's gen is.
- Dead `_flags` fallbacks: **112** in the integration generation vs the **76**
  ratchet the working generation meets. 36+ conditional branches that can never
  be taken is a plausible source of wrong game logic, not merely dead code.
- The working gen still carries 232 unresolved stubs and `[ICALL] skipped
  not-code target` events appear in logs.
- Fixed this session: `_pass_gap_prologues` was splitting table-reached switch
  dispatchers (`sub_00154D70`, `sub_0015A020`), emitting stubs that popped the
  wrong byte counts. `gap_prologue` finds dropped 363 → 87, with 259 addresses
  correctly reattributed. Committed in `95c1d82`.

---

## 6. Root-cause hypotheses, ranked by evidence

| # | Hypothesis | Evidence | Confidence |
|---|---|---|---|
| H1 | Tutorial cannot progress because of **translated game logic**, not rendering | 112 vs 76 dead `_flags`; unresolved stubs; the same class of defect already caused a startup fault this session | **medium-high** |
| H2 | macOS foreground/activation is missing app bundling | no `Info.plist`, no activation policy call, bare Mach-O | **high** |
| H3 | Slowness is wait/spin/logging, not raster throughput | 74% main-thread sleep; 358 M spin acks; 11 GB/15 min | **high** |
| H4 | Silence is early termination of the voice walk or wrong voice-active state | `if (!active) break;`; all surrounding counters healthy | **medium (untested)** |
| H5 | Silence is caused by the `0x001A308E` override returning from a wait that guarded a voice update | the override is unconditional; timing-sensitive by construction | **medium (untested)** |
| H6 | Graphics gaps are `ARRAY_ELEMENT16` + programmable VS | histogram — **but from the wrong sink**, see §5.1 caveat | **unproven** |

H5 deserves emphasis because it implicates my own fix. The override completes a
handshake unconditionally; if the guarded state genuinely is not ready, the
freeze fix and the silence are the same bug seen twice. H4 and H5 are
distinguishable — see P2-T1.

---

## 7. Prioritized goals

The brief's ordering is kept, with one evidence-driven adjustment argued below.

- **P0 — make tutorial/gameplay logically progress correctly**
- **P1 — make rendering correct enough to play**
- **P2 — restore audible sound**
- **P3 — reach playable speed**
- **P4 — normal macOS foreground/window behaviour**
- **P5 — compatibility, accuracy, cleanup**

### Two proposed adjustments

**(a) Pull the cheap half of P3 forward, ahead of P1.** Not the Metal
migration — only the three measured wins in §5.2: silence the logging, fix the
ack spin, examine the wait granularity. They are small, low-risk, and they make
*every* subsequent experiment faster and its logs readable. An 11 GB log is
actively obstructing P0 and P1 diagnosis. This is not "speed first"; it is
removing measurement obstruction before diagnosing.

**(b) P4 is independent and cheap.** It touches no emulation code and can be
done by anyone at any point. Keep it at P4 by priority, but it need not block.

Everything else stays. In particular the brief is right that **P0 before P3**:
if the tutorial cannot be completed for logic reasons, a faster renderer
changes nothing.

---

## 8–9. Tasks, with required evidence

Format per the brief: observed problem · evidence · subsystem · reference ·
smallest experiment · success criterion · regression risk.

### P3-early — remove measurement obstruction (do first)

**T-A · Logging volume**
- *Problem:* 11 GB in 15 min makes runs unreadable and costs real time.
- *Evidence:* §5.2(3).
- *Subsystem:* `src/nv2a/nv2a_pusher.c`, `nv2a_pb_exec.c` periodic reports.
- *Experiment:* find what still prints at `XBOX_LOG_LEVEL=0`; put the
  per-method and `[NV2A]` spam behind an explicit opt-in.
- *Success:* a 10-minute gameplay run produces < 50 MB with the same
  `[GPU]`/`[PUSHER]` summary lines still present.
- *Risk:* low. Risk is *losing* diagnostics — keep every counter, gate only
  per-event lines.

**T-B · `nv2a_ack_thread` spin**
- *Problem:* 358 M no-op iterations vs 325 K useful acks.
- *Evidence:* `[PB-ACK] 303188 loops/s`.
- *Subsystem:* `nv2a_ack_thread`.
- *Experiment:* measure wall-clock frame rate before/after adding a wait rather
  than a spin; do not restructure ownership.
- *Success:* `already=` counter falls by >10× with triangles/sec not worse.
- *Risk:* **medium** — a wait that is too coarse could stall the pusher. Needs
  the before/after triangle rate as a guard.

**T-C · Wait granularity**
- *Problem:* main thread 74% in `Sleep → nanosleep`.
- *Evidence:* §5.2(1).
- *Subsystem:* `bridge_KeWaitForSingleObject`, `Sleep`.
- *Reference:* upstream `be52f25`, `8146ca2`, `e3caa37` — interrupt/DPC/vblank
  delivery. Read these before changing anything; the guest may be waiting on an
  event we never deliver, in which case the fix is delivery, not shorter sleeps.
- *Experiment:* log *what object* is waited on and for how long.
- *Success:* the dominant wait is named and attributed.
- *Risk:* low (measurement only).

### P0 — tutorial progresses

**T-D · Decide logic vs rendering**
- *Problem:* tutorial cannot be progressed reliably; cause unattributed.
- *Evidence:* user report; H1.
- *Experiment:* attempt the instructed action ("press A, jump once") and record
  whether the *game state* advances — sound cue, counter, script step — even if
  drawn wrongly. State advancing with bad visuals ⇒ rendering. State not
  advancing ⇒ logic.
- *Success:* one unambiguous answer. **This gates P0 vs P1 ordering.**
- *Risk:* none.

**T-E · Executed dead `_flags` branches**
- *Problem:* 112 vs 76 fallbacks; branches that can never be taken.
- *Evidence:* §5.5.
- *Reference:* `RECOMP_UNRESOLVED_FLAGS=1` (`startup_probe.c:562`); the
  `backport_*.py` family precedent.
- *Experiment:* run the ratchet audit against the **working** gen and check
  whether any dead site is *executed* during the tutorial.
- *Success:* list of executed dead branches, or a demonstration that none are.
- *Risk:* none (measurement).

### P1 — rendering

**T-F · Identify the production sink (gates all of P1)**
- *Problem:* §5.1's histogram may describe a path we do not render with.
- *Experiment:* determine which counter belongs to the CPU-executor/Metal path
  and re-read the unhandled set from it.
- *Success:* an unhandled-method list attributable to the drawing path.
- *Risk:* none. **Do not start T-G before this.**

**T-G · Close the top real gap**
- Deferred until T-F names it. If it is `ARRAY_ELEMENT16`/programmable VS, note
  that `src/d3d/d3d8_vsh.c` and `nv2a_ff.c` already exist — the likely work is
  *routing*, not implementation.

### P2 — audio

**T-H · Distinguish H4 from H5**
- *Experiment:* run with `RECOMP_AUDIO_COMPLETION_TRACE=1` and the three
  surviving probes (`0x001A25CA`, `0x001A2FBE`, `0x001A3A8E`). If `0x001A2FBE`
  — the genuine bit-15 clear — never fires, the override is masking a handshake
  that never happens (H5). If it fires normally, look downstream (H4).
- *Success:* H4/H5 separated on evidence.
- *Risk:* none. **Do not reintroduce a blocking wait to make the deadlock
  return** (explicit brief constraint).

**T-I · Find where samples go to zero**
- *Experiment:* instrument at exactly two points — mixer output, and the buffer
  handed to `SDL_QueueAudio` (`apu_sdl2.c:84`) — and report peak amplitude.
- *Success:* the first stage producing all-zero samples is named.
- *Risk:* none.

### P4 — macOS behaviour

**T-J · Bundle and activate**
- *Evidence:* §5.4.
- *Experiment:* minimal `.app` with `Info.plist`; if still wrong, add an
  explicit activation policy call.
- *Success:* app appears in front on launch; Dock click raises the window.
- *Risk:* low, and fully isolated from emulation.

---

## 10. Explicitly NOT to work on yet

- **Porting anything from upstream xboxrecomp.** We already contain
  `upstream/main`. There is nothing to port (§0).
- **Rewriting the renderer, or migrating to Metal as the production path.**
  Not justified until §5.2 is acted on and T-F names the real gap. The evidence
  says the current bottlenecks are waits, spins and logging.
- **Implementing NV2A features not in the trace.** Per the brief.
- **A DirectSound HLE boundary** (`xbox_dsound`). Large, and upstream's own gap
  analysis admits streaming is incomplete — JSRF's CRI/ADX path needs exactly
  that. Revisit only if P2 shows the guest XDK path is unrecoverable.
- **Re-litigating `sub_001A308E`** — unless T-H shows the override causes the
  silence.
- **Changing the 76 dead-`_flags` ratchet.** It is a real gap, not a threshold
  to relax.
- **Regenerating the integration tree's gen and expecting it to render.** It
  does not. The baseline uses the dirty tree's gen.

---

## 11. Housekeeping surfaced during review

**Git state is not safe.** Two concrete risks:

1. `codex/upstream-integration` has **never been pushed** — the only remote
   branch is `origin/main`. Every Metal/renderer/audio commit and today's tag
   exist on one disk. iCloud Drive sync is not a git backup.
2. The dirty main tree has **51 uncommitted modified files** on `main` — and it
   holds the *only* generated code that reaches gameplay.

**Contribution candidates**, if wanted:

- To **xboxrecomp** (MIT, accepts outside PRs — see merged #29/#30/#31): the
  `_pass_gap_prologues` fix in `tools/disasm/functions.py` is platform-neutral,
  fixes a real defect in their code, and ships with tests. Cleanest candidate
  by far. The macOS/Metal port is a bigger conversation — their documented
  target is Windows/MSVC, so ask first.
- To **JSRF-Decompilation**: our characterisation of `sub_00154D70` /
  `sub_0015A020` as table-reached switch dispatchers, and of the DSOUND
  completion semantics at `0x001A2FBE`, are facts that repo does not have and
  does track. Note: no `LICENSE` file was found in that clone — check licensing
  before contributing.

---

## 12. The one thing to do next

**T-D.** Attempt the tutorial action and observe whether game *state* advances.

It is free, it takes one minute, and it decides whether P0 or P1 leads. Every
other task is cheaper to sequence once that is known.
