# JSRF goals — the guest stops talking, 17 September 2026

Supersedes `JSRF_GOALS_2026-09-16_NIGHT_THE_DEFECT_LIST.md`. The **target is
unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

What changed today: the frame-time question is **measured and answered**, and
the music question turned out to be a different defect from the one we were
chasing. Section R carries every refutation forward and adds four.

## Where we actually are

| | measured | source |
|---|---|---|
| frame mean | 16.79 ms (59.6 fps) | scripted, 240 s, METAL_FF=1 |
| frame median | **18.5 ms** — above the 16.68 ms budget | same |
| frame p99 | 40.0 ms, max 219 ms | same |
| sync, per frame | p50 **9.0** ms, p99 16.0 ms | `[SYNC]` |
| frame without sync | p50 **8.0** ms, p99 **29.5** ms | `[NOSYNC]` |
| fences | fixed, player-confirmed | — |
| sound effects | audible, player-confirmed | — |
| music | **dies ~2 min in, every session** | player, 17 Sep |
| text | **corrupt in speech boxes AND trick names** | player screenshots |

Two conclusions the measurements force:

**Sync owns the median, not the tail.** `[NOSYNC]` p50 is 8.0 ms, so removing
the surface-swap stall reaches 60 fps. Its p99 is 29.5 ms against `[SYNC]`'s
16.0, so it will **not** fix the hitches. Do not promise that it will.

**The music defect is not the idle trap.** The trap coexisted with a healthy
guest for 26 reports. The guest then stopped issuing APU methods while its own
audio thread kept ticking. That is the defect, and its cause is unknown.

---

## G1 — Find out why the guest stops issuing APU methods

Two minutes into gameplay, every session, `[APU-VOICE] guest_methods` freezes
and never moves again. Around it, everything stays alive: `[ADX] tick`
advances, `[FRAME] flips` advances, all four interrupt vectors keep being
delivered, and the sound engine keeps running. `2D heard` freezes with it, so
the music dies. We then raise the idle trap 41,667 times into a guest that
never answers.

**The sharpest clue.** Something un-traps the front end **38,929 times**
(`[APU-FRAME] episodes`, mean run 7.5 frames) while `guest_methods` stays
frozen. So whatever acknowledges the trap is **not** the thing that issues
methods. Those are different actors and one of them is still alive.

**DirectSound is not ours to refactor.** It is the title's own XDK code,
statically linked into the XBE and recompiled — the ISR at guest `001A24BE`.
`src/audio/dsound_device.c` is a 386-line stub that is not the active path.
The only two levers are what the **APU model** and the **kernel** present to
it, which usefully bounds the search.

**Next action, and it is a measurement, not a theory:** record the calling
thread on the guest-method path (`g_apu_guest_method_count`, `apu_vp.c:786`)
and on the trap acknowledgement. There is no thread-id plumbing there today.

**Done when:** one player session says which of three worlds we are in —
same thread and blocked (look at the kernel object it waits on); different
threads and the method thread stopped (look at why); or the method thread is
running and simply not calling (then our APU model put it in that state).
Each branch leads somewhere different, which is the property the three
hypotheses that died today all lacked.

## G2 — The glyph index error, inside a font batch

Caught on camera at last: `"Let's see how much air $ou can grab"` — `y`
rendered as `$`. That is **one glyph's quad drawn with another glyph's texture
coordinates inside a single batch**, which is the one hypothesis the 16 Sep
handover recorded as untested. Four earlier hypotheses are dead by measurement
(section R).

It is **wider than the tutorial**: trick-name overlays show it too (`arside
Mi*sou`, a mark over the F in `Farside Stab Soul`), plus small floating marks
above speech lines. So look at whatever indexes a glyph quad, not at the
tutorial's own code.

**Done when:** the player sees clean text across a session, and a scripted
pixel diff of a text frame between CPU and GPU arms is empty. 98 candidate
frames are already in `glyphdump/` from 17 Sep.

## G3 — The frame tail: extend `owes_guest_ram` to rendered content

Justified by G3's own instrument (`[NOSYNC]` p50 = 8.0 ms). The cost is the
surface swap: 18,942 of them against 9,504 flip syncs, and 18,937 of 18,942
are rebinds at a **100% cache hit rate**. The machinery to defer the writeback
already exists for clears and ran zero times in a whole run.

**A dependency the previous plan did not name.** `nv2a_pb_exec.c` defines

    #define nv2a_gpu_sync_range(target, bytes) nv2a_metal_sync()

— it takes the range and throws it away. `snapshot_surface()` passes the real
flipped range, but Metal only writes back the **bound** surface, and the live
surface is never the one being flipped. So deferring the swap's writeback
without a range-aware sync presents **stale pixels**. Build
`nv2a_metal_sync_range` first; D3D11 already has the shape at
`nv2a_d3d11.c:1224`.

**Done when:** median frame under 16.68 ms in a mission, verified with the
`[d3d8_gl]` blit check. Expect p99 to stay near 29.5 ms — that is a separate
hunt and not a failure of this work.

## G4 — Resolve or exonerate the `RECOMP_SYNC_HIST` halt

Accumulating unconditionally, a 240 s run stopped the guest dead at 9,480
flips — flips, `guest_methods` and `2D heard` frozen together, emulator threads
still at 389% CPU — where the control at HEAD ran to 13,398 clean. Every prior
run prints its final flip count once; that one printed it eight times.

But it is **one run per arm**, which `ab_score.py` voids, and the code is two
counter reads and arithmetic with no lock, allocation or Metal call. Shipped
**off** for that reason.

**Done when:** two runs per arm settle it. If it is a pre-existing
intermittent hang, that is a bigger finding than the instrument.

## G5 — Delete or re-word `[APU-POOL] on_2d`

It was added to answer "is the music death ours", with the rule *both frozen =
nothing to fix*. Both froze and the game had played music. `on_2d` counts the
guest **starting** a 2D voice, and a stream starts one voice then feeds it, so
it flatlines during healthy playback, silence and mid-flight death alike.

**Done when:** it is gone, or its line says what it can and cannot separate.
The pair that works — `guest_methods` frozen while `idle_trap` climbs — is
already printed.

## Carried forward, unchanged and untouched today

- **G6** the ADPCM cause (the guard makes it silent, not correct)
- **G7** the page-table bound that cannot fire; count before enforcing
- **G8** the PCM stereo page straddle (latent: failing voices are mono)
- **G9** 128 switches still hand-roll their grammar; ratcheted, migrate at leisure

---

## R — Refuted. Do not re-derive these.

| Idea | How it died |
|---|---|
| The flip sync is the cost | Per-caller counters: the flip's sync is already clean, 9,490 of 9,504. The surface swap is the cost. |
| Recovering deferred vblanks is worth ~4% | Guest ISR ran at 101 Hz against 59.94; re-delivering manufactures a tick. |
| ADPCM stride from the segment descriptor | A/B printed the counter in neither arm; the scene has no streaming ADPCM voice. |
| `samples_per_block > 1` breaking the block index | `oversize=0` in every archived run. |
| Overlapping ring reservations from a second thread | 0 draws from another thread. Single-thread assumption now verified. |
| Vertex data changing under the glyph batches | Zero changes across every font-page batch. |
| q ≤ 0 texcoords drawn where the CPU drops them | 0 in 445,498 batches, with a test proving the counter can fire. |
| **The music death is dropped idle-trap interrupts (G7's pending flag)** | **`[IRQ-VEC]` shows v1, v3, v5, v6 all delivering steadily PAST the death. The ISRs are running.** |
| **`g_vector_in_service` leaked, so the APU vector defers forever** | **Same evidence. Nothing is stuck in service.** |
| **Trapping the front end idles the sound engine** | **`[APU-FRAME] trapped_skipped=0`, and `se = total − halted` exactly. `RECOMP_APU_SE_WHILE_TRAPPED` would not have helped.** |
| **The idle trap triggers the guest freeze** | **The trap ran from report 19 at ~360/report with the guest issuing ~500 methods/report alongside it. It froze at report 26.** |

## Rules for this phase

- A player-facing default needs a picture or a listen, not a residual.
- One run per arm is a lean, not a measurement.
- **Take the refuting measurement before building the theory.** Three
  hypotheses died on 17 Sep inside an hour, each killed by a counter already
  in the log before the theory existed. "Measure, don't infer" is also a rule
  about the *order of operations*.
- Scripted runs did not catch the glyph defect in twelve attempts; one human
  session caught it repeatedly. For anything needing two minutes of real play,
  **the player's session is the instrument**.
- `pgrep -x jsrf_first_fault` before touching `src/`.
