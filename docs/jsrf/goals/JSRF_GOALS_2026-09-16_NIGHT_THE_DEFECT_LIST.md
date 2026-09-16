# JSRF goals — the defect list, 16 September 2026 (night)

Supersedes `JSRF_GOALS_2026-09-16_EVERYTHING_ON_THE_GPU.md`. The **target is
unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

What has changed is that a day of hunting turned a vague list into a specific
one. Every goal below has a done-criterion that a run can settle, and section
R lists what is REFUTED so nobody spends an evening re-deriving it.

## Where we actually are

| | measured |
|---|---|
| frame mean | 15.83 ms — **the 60 fps mean is met** |
| frame median | 17.5 ms — **above the 16.68 ms budget, so half of gameplay frames still cost a simulation step** |
| p99 | 39–43 ms |
| fences | fixed, player-confirmed |
| sound effects | audible, player-confirmed |
| music | re-raise fix works in scripted runs; a player session still froze the counter |
| text | two glyphs relocated, once in 24 captures, GPU fixed-function path only |

Speed is now a **tail** problem, not an average one. That reframes G3.

---

## G1 — Music never stops, and we can prove which side stopped it

The bounded re-raise (`RECOMP_APU_IDLE_TRAP_REARM_MS`) is in and measured:
trapped 53% → 3.6%, 2D heard the highest of the three arms, no voice theft.
But the player's 18:33 session still froze the 2D counter at 95,324 for 26
windows, and **nothing in the log could say whether that was our defect or a
stretch of the game with no music** — which is why `[APU-POOL] on_2d` now
counts the guest *asking* for a music voice.

**Done when:** a player session of 5+ minutes shows `on_2d` and `2D heard`
moving together, or shows `on_2d` climbing while `2D heard` is frozen — at
which point the handles in the idle ring name the voice we dropped. Then
default the re-raise in the tree.

## G2 — Everything on the GPU, drawing the same picture

`RECOMP_METAL_FF` is on in the player's config. One real divergence was found
and closed (the GPU path skipped the CPU path's clip-w batch refusal). The
glyph corruption itself is **timing-dependent, not data-dependent**.

**Done when:** the player confirms clean text on a build with the switch on,
and a scripted pixel diff of the tutorial text frame between arms is empty.
Then make it the tree default.

**Next action:** the label-region trap (running). The speaker's name label is
static while dialogue advances and changed *exactly once* in the GPU arm — on
the corrupt frame — so any dump there is a candidate rather than noise.

## G3 — The frame tail, which is where the speed now lives

The flip sync is 4–9 ms per frame, one call per frame, ~70% of all sync cost,
and is **untouched**. The frame is serialised into submit → stall the GPU →
read 614 KB back → convert → present, and exists only because the presenter
is OpenGL and reads the guest framebuffer out of guest RAM. The live Metal
surface is **never** the surface being flipped, so the design must look the
flipped address up in the surface cache.

**Order, and the first two are instruments:**
1. Fix the blit check's sample point — compare against the expected bilinear
   blend with a 1 LSB tolerance. It currently means "should be close", not
   "must be equal", because the drawable is 4× the guest on a 2× panel.
2. A per-frame sync histogram. Removing `waitUntilCompleted` removes the
   serialisation, not the GPU work, so it may move the median and leave the
   tail alone — and the median is the thing we need.
3. Then the surface-cache lookup, then the presenter.

**Done when:** median frame time is under 16.68 ms in a mission, with p99
under 20 ms.

## G4 — The ADPCM decoder stops emitting stack memory as audio

27,801 refusals per 150 s, each one handing the mixer an **uninitialised
stack array at full scale**. Established: block 0 of a buffer never fails,
middle blocks fail 26,314 times, and the "header" being rejected is ADPCM
sample data (`08080808`), so we are reading the wrong bytes rather than
corrupt ones.

**Done when:** `fail` is at or near zero in a gameplay run, *or* the guard is
defaulted on so a refused block yields silence rather than stack. The guard
(`RECOMP_APU_ADPCM_GUARD`) already exists and is cheap — **if the cause
resists, ship the guard anyway**; emitting uninitialised memory as audio is
not acceptable while the investigation continues.

**Under test now:** failures may begin past the first page (4096/36 = 113.8
blocks per page; every observed failing block is above 113).

## G5 — The page table gets a bound that can actually fire

`get_data_ptr` takes `max_sge` and asserts on it; all three call sites pass
`0xFFFFFFFF`, so the assert is dead code and a read past the end of the voice
processor's page table silently returns a translation built from whatever
dword sits there. The hardware has real MAXSGE registers.

**Done when:** an out-of-range translation is counted and refused rather than
performed. **Do not simply enable the assert** — an assert that starts firing
in a player's build is a crash, and we do not yet know whether the guest
legitimately walks past the table. Count first, then decide.

## G6 — Per-read translation everywhere it is needed

The PCM sample path translates once per sample then walks channels on the
translated *physical* address, so a stereo sample straddling a 4 KB boundary
reads its second channel from whatever page physically follows. The ADPCM
path re-translates per dword, which is the intended discipline.

**Done when:** a counter shows how often a sample straddles a page, and the
loop re-translates when it does. Latent today (the failing voices are mono),
so this is correctness, not a symptom chase.

## G7 — Delete or implement the pending-interrupt flag

`g_vector_pending` is set, cleared, counted for a statistic, and read by a
*test* accessor. **No production code re-delivers on it**, so its comment
("Pending, not dropped… retried within one period") is false. It cost me an
evening: I built a recovery on that premise and had to retract it (section R).

**Done when:** either it has a consumer, or it and its comment are gone and
the deferral is documented as a drop.

## G8 — Commit the day's work

**1,650 uncommitted lines across 15 files.** A day of fixes and instruments
with no commit is one bad command from gone.

**Done when:** committed in logical pieces — the fence default, the mixdown,
the re-raise, the GPU fixed-function parity, the instruments, the harness.

---

## R — Refuted. Do not re-derive these.

| Idea | How it died |
|---|---|
| Recovering deferred vblanks is worth ~4% of game speed | Built it. Guest ISR ran at **101 Hz** against a 59.94 Hz hardware rate, delivered exceeded raised by 5,380, frame time unchanged. The source is already asserted when a delivery is refused, so the running handler services it; re-delivering **manufactures** a tick. `delivered` counts handler *entries*, not simulation steps. |
| The 5% frame-rate win from that switch | Noise. The next pair's *off* arm read 63.2 fps against the first pair's 59.4. One run per arm cannot see 5% on this host. |
| ADPCM stride comes from the segment descriptor | A/B: 3.83% → 4.13% failures, and the counter printed in **neither** arm — the scene has no streaming ADPCM voice at all. |
| `samples_per_block > 1` breaking the ADPCM block index | `oversize=0` in every archived run proves `spb == 1` for the failing mono voices. |
| Overlapping ring reservations from a second thread | The draw entry saw **0 draws from another thread**. The single-thread assumption the ring, texture cache, `last_command` and all surface state rest on is now *verified* rather than asserted. |
| Vertex data changing under the glyph batches | Zero changes across every font-page batch, both arms. |
| q ≤ 0 texture coordinates drawn where the CPU drops them | **0** in 445,498 batches. |

## Rules for this phase

- A player-facing default needs a picture or a listen, not a residual.
- One run per arm is a lean, not a measurement.
- When a number does not fit, look for a reason the *hypothesis* is wrong
  before a reason the *number* is wrong. Both can be true; I hit that exact
  trap tonight and the counter fix is what refuted the theory.
- `pgrep -x jsrf_first_fault` before touching `src/`, and remember the
  harness `rm -rf`s its own output directory at start.
