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

**NARROWED 17 Sep, from the player's own log, with no new code.** The
ambiguity `apu_vp.c:781` admits it cannot resolve — "a guest that stopped
submitting" versus "writes lost before reaching this entry point" — is settled
by a counter that already existed upstream of the entry point:

    [MCPX-TRAP] faults=1404969 apu=222538 vp=23212 ack_windows=0
                reprotect_failures=0 | mcpx aliased=319075 windows=0

`vp` is frozen at 23,212 and **equals `guest_methods` exactly**, so every
trapped VP write became a method and nothing is being lost. `apu` keeps
climbing at ~1,680 per report and `faults` keeps climbing with it.

So: **the guest is alive, still faulting on the aperture, still writing APU
registers — and has stopped writing to the voice-processor region (0x20000–
0x30000) altogether.** This is not our write path. Both apertures are aliased,
`ack_windows=0`, `reprotect_failures=0`, `windows=0`: every guard hazard
CLAUDE.md warns about is clean.

It also identifies who un-traps the front end 38,929 times while
`guest_methods` is frozen — the guest, through the main APU registers. It is
acknowledging our traps and declining to submit voice work. So DirectSound
believes it has nothing to submit, and the question is what our model told it
that makes it believe that.

**Next action, and the instrument already exists:** `RECOMP_APU_WRITE_TRACE=1`
prints `[APU-WRITE] main= vp= gp= ep= other=`, which says WHICH registers the
guest is still writing while the VP region is silent. Add it to the player's
`paths.conf` for the next session. Thread-id plumbing on the method path is
the follow-up if that is not enough, not the first move.

**Done when:** we know which APU register the guest's DirectSound is
polling while it refuses to submit, and what our model is answering. The
third of the three worlds is now the one we are in — the guest is running and
simply not calling — so the defect is a value our model presents, not a lost
write and not a dead thread.

## G1a — Make `RECOMP_APU_FEDEC_HOLD` the default *(player-confirmed fix)*

**The first player-confirmed fix of 17 Sep.** With the guard off the title
crashed inside the guest's DirectSound ISR shortly after New Game, with a
guest stack ending in `001A25D9` and 62 of 2,555 guest methods dispatched
while the front end was TRAPPED. With `RECOMP_APU_FEDEC_HOLD=1` the next
session ran to completion: `held=1702` of 1,702 such methods, **no crash**.

The guard holds the FEDECMETH/FEDECPARAM pair still while the front end is
trapped, which is what the hardware does — a trapped front end has stopped
decoding. Without it a guest method landing between the ISR's two MMIO loads
hands it our `0x8000` with somebody else's argument; `SET_ANTECEDENT_VOICE`'s
argument is a voice handle, so it passes the ISR's `h >= 0x100` guard and is
dereferenced.

It has been **off by default since it was written and never once exercised**,
and `apu_vp.c:1793` claimed "default on" until today while the accessor read
`hold = e ? (atoi(e) != 0) : 0`. The report line had already been corrected;
the comment had not, and the comment is what a reader reaches first.

**Done when:** defaulted on in the tree, with a test that drives `held`
non-zero and a second that shows it staying zero when the front end is not
trapped. One player session is the confirmation; a second would make it two.

## G1b — Why do v1 and v3 never clear the idle condition?

The trap storm is **two voices and nothing else**: v1=1,783 raises, v3=1,730,
every other voice ≤2, alternating `1 3 1 3 1 3` forever. The guest services
each raise — `[APU-WRITE] main` climbs ~1,750 per window against ~360 raises,
about five register touches per raise — and the condition never clears.

**NOT a voice-list cycle.** `[APU-CYCLE] walks_with_a_cycle=0` and
`[APU-WALKCAP] hit=0`. The detector marks each visited voice in a per-walk
bitmap and never saw a revisit, so no walk ever met a ring.

*How that hypothesis died, recorded because the mistake is reusable:* the
idle-trap detail lines aggregate to `3D:v3[]<-v1` 71 times and `3D:v1[]<-v3`
8 times, which reads like a mutual link. Those are separate raises at
different moments, and the links simply changed over time. Aggregating a
per-event field across a whole run and reading it as simultaneous state is the
same class of error as comparing across scenes.

**Done when:** we know what about v1 and v3 reads as inactive-and-linked
forever. Their trap flags are consistently `[]` — not `L` (locked), not `N`
(never VOICE_ON), not `P` (ISR returns early), not `R` (repeat) — which is
itself a clue, since every documented reason for a persistent raise has a
flag and none of them is set.

## G1c — The music decays, it does not cut out

The player's words were "the in game music slow and then stops", and
`[APU-BIN] 2D heard` says the same thing per window:

    +15008  +14992  +14476  +13896  +13610  +8736  0  0  0 ...

A progressive decay over roughly thirty seconds, then silence. **It is not the
APU falling behind:** `[APU-FRAME]` is steady across the same windows at
~7,500 frames per window, `se` 91–95%, `trapped` flat at 32–36%. So the mixer
is getting its frames and producing less and less 2D audio from them.

**Done when:** we know what declines. A decay rules out an abrupt voice kill
and points at starvation — fewer 2D voices contributing per frame, or one
voice being fed less. The per-voice breakdown is what separates those.

## G1d — The storm precedes the collapse, and is the prime suspect again

The 09:43 session has what the 09:07 session did not: a **pre-storm baseline**.

    window  2-9: idle_trap +0 (a few at 4-5)   guest_methods +250..1450   healthy
    window 10:   idle_trap +261                guest_methods +2931        storm begins
    window 11:   idle_trap +348                guest_methods +2244
    window 12:   idle_trap +365                guest_methods  +540        3D dies
    window 13:   idle_trap +366                guest_methods  +519        2D decaying
    window 14:   idle_trap +370                guest_methods  +537        2D silent
    window 16:   idle_trap +343                guest_methods    +0        guest gives up

Eight quiet windows, then the storm arrives and the guest is dead within six.
Note windows 10 and 11: the guest works **four times harder than baseline**
before it collapses. That is a system being driven past its capacity, not one
that wandered off.

And the audio dies in a specific order — **3D stops dead at window 12, 2D
decays out over the next two** — which is what a 2D voice playing out an
unrefilled buffer sounds like, and is exactly the player's "slow and then
stops". The storm voices v1 and v3 are on the **3D** list, and 3D is what dies
first.

**This does not prove causation** — the storm and the collapse could share an
upstream cause, and one session is one session. But the ordering is now clean
in the arm that has a baseline, so the storm is the prime suspect again rather
than a refuted one.

**Done when:** a second session reproduces the ordering, and we know what
makes v1 and v3 stay inactive-and-linked (G1b). If the cause resists,
`RECOMP_APU_IDLE_TRAP_REARM_MS` bounding the re-raise per voice is the
mitigation to A/B — but it re-opens the "voice never reclaimed" failure the
re-raise was added to fix, so it is a trade, not a fix.

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
| **The v1/v3 idle-trap storm is a two-voice list cycle** | `[APU-CYCLE] walks_with_a_cycle=0`, `[APU-WALKCAP] hit=0`. The "cycle" was `v3<-v1` and `v1<-v3` aggregated across a whole run — separate raises at different moments, not simultaneous state. |
| **The music "slowing" is the APU falling behind** | `[APU-FRAME]` steady at ~7,500 frames/window, se 91–95%, trapped flat 32–36%, across the exact windows where `2D heard` decayed from +15,008 to 0. |
| ~~The idle trap triggers the guest freeze~~ **RETRACTED — see G1d** | This was refuted on the 09:07 session, where the storm was ALREADY RUNNING before the window I examined, so "the guest coexisted with it" was an artifact of starting to look too late. The 09:43 session has a clean pre-storm baseline and reverses it. |

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
