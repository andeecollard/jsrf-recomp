# What actually moves the needle — 17 September 2026, evening

Written after a day in which three player-visible defects were worked on and
**none of them was fixed**. This is an analysis of why, and a plan that does
not repeat it.

## 1. The only scoreboard that counts

| defect | player sees | status tonight |
|---|---|---|
| music dies | stops ~2 min in, every session | **open** |
| text wrong | label flickers; glyphs mis-render | **open** |
| speed | frame median 15.5–18.5 ms vs 16.68 budget | **open** |

Everything else landed today — a crash fix, a measurement, a resolved A/B, two
upstream bugs, two dead instruments retired — is real but invisible to the
person playing. By the scoreboard above the day produced nothing.

## 2. Why the day went that way

**One fix landed and it was the one nobody was looking for.** The crash after
New Game was diagnosed and fixed in about twenty minutes because the log
already said `62 of 2,555 guest methods dispatched while TRAPPED` and the
guard for it existed and was off. Short path: symptom → existing counter →
existing switch → confirmed by the player.

**Five theories died on the music.** Dropped interrupts; a leaked in-service
flag; trapping idles the mixer; a two-voice list cycle; the self-link fix.
Every one was killed by a counter, and **four of those counters already
existed in the log before the theory was formed**. The pattern is not bad luck.
It is a method error: reading mechanism out of source, then looking for a
number to support it, when the number was already there to refute it.

**And the measured, justified, unblocked work sat untouched all day.** G3 was
ranked second this morning with a completed measurement saying GO —
`[NOSYNC] p50 = 8.0 ms` against an 18.5 ms median. It needed no player, no new
theory, and no session time. It was never started, because every play session
produced a new audio theory that felt more urgent. Reactive debugging crowded
out the work that was already de-risked.

## 3. The structural finding: two defects, one root

`snapshot_surface()` is the presenter's read of guest RAM. It passes the real
flipped range:

    nv2a_gpu_sync_range((uint8_t *)mem + s_gpu.color_offset,
                        (size_t)s_gpu.pitch * (s_gpu.clip_y + s_gpu.clip_h));

On macOS that macro is:

    #define nv2a_gpu_sync_range(target, bytes) nv2a_metal_sync()

**The range is discarded.** `nv2a_metal_sync` writes back exactly one surface —
the currently BOUND one — and only when `surface_dirty`. The title rotates
surfaces constantly (402,784 rebinds in one session at a 100% cache hit rate),
and the superseded G3 plan already recorded that *the live surface is never the
one being flipped*.

So the presenter asks for the pixels at address X and receives whatever surface
happened to be bound. When that is not the flipped surface, guest RAM at X is
stale — **which is precisely what a label present on one frame and absent on
the next looks like.**

The same missing capability is G3's blocker. The swap currently pays a full
drain and read-back every time *because* there is no way to write back a
specific range later; deferring it needs a range-aware writeback to be safe.

**One piece of work therefore addresses a correctness bug the player can see
and 48% of the frame.** That is the needle.

## 4. The plan

### Track A — range-aware surface writeback *(do this first, uninterrupted)*

**A1. `nv2a_metal_sync_range(uint8_t *target, size_t bytes)`.** Walk the
surface cache; write back every slot whose guest range overlaps. Mirror
`nv2a_d3d11.c:1224 sync_range_inner`, which already does exactly this and is
the proof the shape is right. Then point the macro at it so the presenter's
range stops being thrown away.

*Payoff:* the presenter gets the surface it asked for. Candidate fix for the
flicker, and a correctness fix regardless of whether it is.
*Risk:* low — it writes back MORE than today, never less.
*Test:* `jsrf_metal_copy_test` plus the `[d3d8_gl]` blit check, which is
exact again as of 16 Sep. Needs no player.

**A2. Extend `owes_guest_ram` from clears to rendered content.** With A1 in
place a swap between two resident surfaces can mark the outgoing slot as owing
guest RAM and skip the drain; the flip and any guest read force it.

*Payoff:* `[NOSYNC] p50 = 8.0 ms` against today's 18.5 ms median. Upper bound,
because removing the stall removes serialisation and not GPU work — but the
median is the number that decides 60 fps.
*Risk:* real. It changes WHEN guest RAM becomes correct. A1 is what makes it
safe, which is why the order is not negotiable.
*Caveat to state up front:* `[SYNC] p99` is 16.0 ms while `[NOSYNC] p99` is
29.5 ms, so this buys the median and **not** the hitches.

### Track B — the audio, with the method changed

Stop generating mechanisms. One measurement, then a decision:

**B1.** A histogram of main-region APU write offsets. The guest writes main
registers ~1,750 per window while submitting no voice work, and nothing in the
tree says which ones. `[APU-MMIO]` samples offsets but only caught GP/EP.

**B2.** If B1 does not settle it, the question to ask is not "what is our model
doing wrong" but "is the handle we hand the ISR one it can act on". Eight
writes to the 3D list head in a whole session, against thousands of raises, is
a guest that acknowledges and declines. Compare our `SE2FE_IDLE_VOICE` payload
against xemu's for the same sequence before theorising again.

**Rule for this track:** no fix ships without a counter that can refute it
*before* the player is asked to test it. That rule is what turned the
self-link theory into a ten-minute refutation instead of an evening.

### Track C — cheap and real, when Track A is blocked

- The upstream mixbin discard: a handful of lines, confirmed present in
  `upstream/main`, silently drops every 3D voice — every sound effect in any
  title. We have a player who confirmed it by ear.
- Re-arm the glyph trap against frame N-2 so a future session is not spent on
  a dead instrument.

## 5. What this plan deliberately does not do

- **No more audio theories tonight.** Five died today. The next audio move is a
  measurement, not an idea.
- **No upstream merge.** 14 conflicts including `lifter.py` and
  `translator.py`, and merging those invalidates every archived gen.
- **No chasing the `$ou` glyph corruption.** It is rarer and cosmetic; the
  flicker is constant and may fall out of A1 for free.
