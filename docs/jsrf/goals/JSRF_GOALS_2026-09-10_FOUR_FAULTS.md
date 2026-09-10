# JSRF macOS — the four visible faults, and how to attack them

Date: 2026-09-10 (Europe/London)
Supersedes the goal-setting in `JSRF_GOALS_2026-09-09_GAMEPLAY.md`, which
predates the xemu reference rig.

---

## 0. The headline, because it changes what to work on

**The four faults are not one fault.** The obvious systemic explanations were
tested and refused, so there is no single fix waiting to be found. Plan the work
as four separate investigations.

The most attractive theory — that the guest's clock is wrong, which would
explain slow playback, choppy audio, missing fades and a stalled tutorial all at
once — is **dead**. Measured over 70 s and 18.7 M triangles:

```
[VBLANK] delivered=4349 over 69989 ms = 62.1 Hz (target 62)
         deadlines=4349 unacked_skips=0 max_gap=17 ms
```

`deadlines == delivered` and `max_gap` 17 ms against a 16 ms period: not one
period was ever missed. The clock is exact. Frames are lost to how long we take
to draw them, not to when we are told to draw.

That is worth internalising before planning: **"slow" here is an ordinary
performance problem, not a timing one.**

---

## 1. The reference rig is the main asset

`diagnostics/jsrf_first_fault/xemu_reference/` — read its README before using
it. xemu runs the same US ISO, so it answers "what should this do?" directly.

Three ways to ask, cheapest first:

| question | tool |
|---|---|
| what should this look and sound like? | `capture_reference.sh` |
| what methods does the title issue here? | gated `-trace` at the monitor socket |
| what changed between two game states? | `mode_finder.py` over the GDB stub |

**Every absence measurement needs a positive control.** This is not a slogan
here; it is the difference between four wasted hours and four findings. `off=0`,
`idle_trap=0`, `rejected=0` and "mode flags all zero" each looked like evidence
and each meant nothing until something known-good was measured beside it.

---

## 2. Ranked goals

Ordered by tractability, not by how annoying the symptom is. The first two can
be iterated without reaching gameplay, which matters: a playthrough per
iteration is the main cost in this project.

### G1 — Intro card fades  *(iterates in ~45 s, no playthrough)*

**State:** colours fixed (`e2e739c`). Fades still cut instead of ramping.

**Known, measured:**
- xemu ramps 5 → 55 → 108 → 170 → 227 over ~1.5 s. We jump 14 → 248, with no
  intermediate value in any transition.
- Draw counts match: ours ~3.0 batches/frame at 60 fps, reference 3.7 at 30.
  We are not dropping the fade's draw.
- The reference issues `DST_COLOR/ZERO` (0x306/0x0) 33 times in the intro — a
  multiply blend, the shape a fade-to-black uses. That pair is outside the set
  `nv2a_texture_copy.c` accepts.
- **Our guest never asks for it.** Over 1996 frames and 5968 batches the only
  combination seen is `src=0x302 dst=0x303 eq=0x8006`, and `rejected=0`.

**So the fade is upstream of the renderer.** Widening the accept set fixes
nothing. Next: find what decides to draw the fade and why it decides not to.
The opening object's draw path is documented in the architecture video
(`Video Notes/nJWHf-uUIBU.txt`) and is small — clear the screen, set a texture,
draw a quad.

**Watch item:** the clear-value fix reads the low 16 bits for 16-bit surfaces.
The guest also issues `0x00231F20`, which only reads sensibly as A8R8G8B8. The
reference never displays the green a low-16 reading would give, so it is
probably on a 32-bit surface — but that is unproven. **If anything renders
bright green, look here first.**

### G2 — Choppy audio  *(measured and bounded; no playthrough to reproduce)*

**It is a delivery fault, not a mixing fault.** Generation is correct at
47983 Hz against a 48000 target. Delivery is not:

```
[APU-SDL2] max_submit_gap=71.0 ms gaps_over_cushion=22
[APU-PACE] starved=15 empty=5 min_queued=0 bytes (0.0 ms)
[APU-SDL2] depth buckets: 0=5 <1=10 1-2=89 2-4=38393 4-8=98375 8+=1838
```

The queue ran **completely dry 5 times**; 22 gaps exceeded the cushion, worst
71 ms. The mixer produces correct audio at the correct rate and then fails to
hand it over on time.

Note the vblank result rules out one explanation: the 71 ms gaps are **not** the
guest losing its clock. Whatever stalls the submitter does not stall vblank
delivery, which never missed a period. That is a useful constraint — look for
something that blocks the submitting thread specifically.

### G3 — Playback speed

Reframed by the vblank measurement into an ordinary performance problem.

Measured: intro ~58 fps, title screen ~41 fps, gameplay ~15 fps, against a
steady 62 Hz clock. So we render too slowly under load, and the game drops
frames the way it should when it cannot keep up.

**Get a clean xemu baseline first.** Every xemu fps figure in the handovers
(10.5–12) was taken with the GDB stub or a pgraph trace active and is not a fair
comparison. Do not conclude anything about relative performance until xemu has
been measured without instrumentation attached.

### G4 — Tutorial: no player, Corn vanishes, no progression

The hardest, and the one that has consumed the most effort for the least
result. **Read §3 before starting** — eight hypotheses are already spent.

**The strongest untested lead**, from the architecture video:

> "There are a few other trees on linked lists used by the draw phase as well,
> not to mention every game object has a few flags that determine how it's used
> in the draw phase."

The scene-graph walk that found 67 live objects and **zero** dead ones traversed
the **exec** tree. The draw phase uses different structures that nobody has
looked at. A character alive in the exec tree but absent from the draw lists
presents exactly as observed: world renders, characters do not, nothing culled,
nothing rejected, every method arriving.

`mode_finder.py` is the tool — diff the game object between a frame where a
character is visible and one where it is not.

---

## 3. Do not re-derive these

Nine hypotheses, each proposed, tested and killed. Full evidence in
`docs/jsrf/handovers/CLAUDE_HANDOVER_2026-09-10_XEMU_REFERENCE.txt`.

| hypothesis | killed by |
|---|---|
| APU/audio front end broken | `idle_trap=11 off=8 release=0` at gameplay, matching xemu |
| executor discards transform/texture state | every aligned method < 0x2000 is stored to `s_methods[]` at `nv2a_pb_exec.c:1729`, *before* the switch |
| corrupted matrix in guest RAM | `RECOMP_FIND_NAN`: "none in RAM", every scan |
| entities destroyed | 67 live, 67 walked, **0 dead** |
| geometry culled by us | 0 batches skipped, 0 triangles off-surface |
| batches rejected wholesale | 640 of 665738 (0.1%) |
| missing depth clear | counter artifact; the Z clear runs at `:845-851`, before the early return at `:854` |
| draw-rate divergence (200 vs 102) | xemu itself swings 102 → 150 between two scenes of one level |
| the guest's clock is wrong | 62.1 Hz, zero periods missed |

**Two traps worth carrying forward:**

- The executor's `unhandled` list does **not** mean values are dropped.
  `note_unhandled()` only counts. CLAUDE.md already flags the D3D11 sink's list;
  this is the same trap in different clothing.
- The game-mode booleans reading all zero is **normal**. Pause sets `+0x3C` and
  `+0x40`; a cutscene sets nothing. An earlier note recorded "all zero for the
  whole boot" in a tone that invited suspicion and cost a session.

### Counters in this tree that lie

Five found misleading in one day. Read a counter's trigger before its value.

| counter | why |
|---|---|
| `off=` | blind to VOICE_RELEASE (now paired with `release=`) |
| `[PUSHER] draws=` | always 0 by construction |
| `[GPU] unhandled=` | "no explicit case", not "value dropped" |
| `[FB] sum=/same` | probes a fixed address; reads a retired surface as zero |
| `[GPU] clears=` | does not count depth-only clears |

---

## 4. Still unexplained, not on the critical path

- **449 batches rejected as `oPos is not finite`**, a count that climbs steadily
  through a run. Too few to hide the characters, but real and nobody has
  explained it.
- The intro presents ~58 fps where xemu presents ~30. The clock is right in both,
  so the title is completing frames twice as fast as the reference. Harmless so
  far, but it is a real divergence and may bear on G1.

---

## 5. Not goals

- **Porting from upstream.** `051a128` is still an ancestor of HEAD; there is
  nothing new to take.
- **Running `tools.abi_analysis`.** It is the one pipeline stage we skip, and
  adding it would *break* the build: definitions are emitted `void(void)`
  unconditionally at `translator.py:1391`, while `output.py` would start
  emitting ABI-derived prototypes. The empty ABI db is the consistent
  configuration.
- **Widening the blend accept set** for G1. Measured: the title never asks.
