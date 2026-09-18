# What is left after four leads died — 18 September 2026

Written after a day that closed more than it opened. Four hypotheses died to
measurement, one result separated cleanly, and the player's three visible
defects are all still there. This is what the remaining evidence supports doing
next, in order.

## The scoreboard that counts

| defect | player sees | status |
|---|---|---|
| music dies | stops ~2 min in, every session | **open**, and narrowed hard |
| text wrong | glyphs mis-render | **open**, best lead dead |
| speed | 19 ms median vs 16.68 budget | **9% found, not enough** |
| crashes | two distinct, one new | **uninvestigated** |

## What died today, so nobody re-derives it

- **"The guest was never told about the dead voices."** `found idle=10,
  delivered=10, never told about=0`, on a session where the music died.
- **"A voice-list cycle is the mechanism."** `cycle_break` on, `broken=0`,
  `walks_with_a_cycle=0` — and the music died anyway. Cycles are real (88 in
  the 22:40 session) but **not necessary**.
- **"The texture matrix convention explains the glyphs."**
  `TEXTURE_MATRIX_ENABLE=0` on all three units; JSRF never uses one.
- **"Unsupported texture formats mis-decode the font atlas."**
  `[TEXTURE] prepared=1548512 rejected=0`.

## Track A — finish G3. It is the only thing converting to player-visible gain.

**A1. Narrow A2's depth refusal and A/B the combination.** `defer_swap` refuses
whenever depth is dirty and that refusal fired 5,747–12,656 times against ONE
successful deferral — it is the whole reason A2 is inert. `no_depth_sync` now
shows the depth write-back is skippable: 9.1% less frame time, ranges not
overlapping, eight usable runs, every `=1` below every `=0`.

Both pieces work in isolation and have never been run together. That is the
combination Track A was aiming at from the start.

*Expect:* more than 9%, less than the `[NOSYNC] p50 = 8.0 ms` ceiling.
*Cost:* one A/B, no player.

**A2. The full-frame diff that is owed.** Zero non-MATCH blit checks across
twelve runs is ~30 sampled checks of ONE pixel each and cannot see a
depth-dependent artifact mid-frame. Until a scene-matched frame comparison
exists, "guest RAM does not need the depth" is evidence and not proof, and
**nothing here should become a default**.

*This gates shipping, not measuring.* Do A1 first; do not ship either switch to
the player until A2 passes.

## Track B — G1, with the question finally narrow

The day's result changed the shape of this. The guest is handed a valid handle
for every voice that goes idle, and removes none of them. From the 09:00
session's trap ring:

    6908 x [R]   repeat of a handle already delivered
      19 x []    no flags at all
       0 x [P]   ISR returned early on CFG_FMT PERSIST
       0 x [L]   voice locked mid VOICE_ON/RELEASE
       0 x [C]   voice in a ring

So it is not PERSIST, not the lock, not a cycle. Nineteen genuinely new raises
naming ten voices, all delivered, all ignored — then 6,908 repeats.

**B1. Capture the removals that DO happen.** `[VOICE-TOP]` counts eight guest
writes to the 3D list head in a whole session. The guest *can* remove a voice;
it does it eight times against thousands of requests. **Instrument those eight**
— what state preceded each one, which voice, and what the guest had just been
told. Eight successes are a far smaller haystack than 103,644 failures, and
they are the only positive examples of the behaviour we want.

*This is the measurement B should start with, and it has never been taken.*

**B2. Only then**, ask whether voice completion is being signalled through the
path DirectSound actually acts on. The idle trap is one notification; a title
normally recycles a voice when its own buffer bookkeeping says so. That is a
hypothesis and it is written here as one — B1 comes first, and B1 may kill it.

**Rule carried forward, and it earned itself again today:** no audio fix ships
without a counter that can refute it *before* the player is asked to test it.
XC_AUDIO was flipped on a strong argument and changed nothing audible.

## Track C — G2, with three mechanisms removed

The fixed-function path for this title is now known to be **a composite
transform and nothing else**: `texgen = 0000` on every unit, no texture matrix,
lighting registers never written, VIEWPORT_SCALE baked into the composite.
So a glyph defect on the FF path has to come from that transform or from the
vertex attribute fetch.

**C1. Re-arm the frame watch against frame N−2.** It has been dead since
17 Sep — it alternates by frame parity and fires every frame catching nothing,
so twelve scripted runs "never caught" the defect. Until this is done, any
future session spent hunting the glyphs is wasted the same way.

*Cheap, and it unblocks everyone else's time rather than mine.*

**C2. A/B `METAL_FF` against the glyphs.** The player runs `METAL_FF=1` and the
goals already record FF drawing corrupt tutorial glyphs about once in 24
captured frames. With C1 done, this is answerable from captures rather than
from a human noticing.

## Track D — the crash nobody has looked at

Two distinct signatures, and only one is understood:

    known    guest 0xFFFFFFBE   sub_001A2E2E <- 001A24BE <- 001A25AA   DSOUND
    new      guest 0xFFFFFFFF   sub_00011EE0 <- 000123E0 <- 0006F9E0   .text

The new one is in **the game's own code**, faulting on `ESI=FFFFFFFF` used as a
pointer, with `ECX=EDI=D95AD9BB` and heap header fields that are not plausible
pointers. It has been seen once. It is the only defect on this list nobody has
spent an hour on.

*Worth one hour, not more, until it reproduces.*

## The upstream debt

1. **PR #67** (mixbin discard) — open, no comments. Nothing to do but wait.
2. **Narrow rotates** — fixed, tested, verified against upstream's own lifter
   by differential fuzzing, pushed to `andeecollard/xboxrecomp:narrow-rotate-width`.
   **PR not opened**; awaiting a decision.
3. **G15** (`bts`/`btr`/`btc` report CF after their own write) — file as an
   **issue**, not a patch. Fixing it means changing how their flag
   reconstruction works, which is not a drive-by change in someone else's tree.
4. **`get_data_ptr`'s dead bound** — issue, same reasoning.

## What this plan deliberately does not do

- **No new audio theory.** Six have died. B1 is a capture, not an idea.
- **No shipping to the player** until the frame diff exists. Two switches went
  out on test evidence without a look in this project's history and both came
  back.
- **No upstream merge.** Unchanged: 14 conflicts, and it invalidates every
  archived gen.
- **No more one-run A/Bs.** Today's two depth A/Bs cost twelve runs to produce
  one usable comparison, because half the control runs never reached a mission.
  Budget six runs per arm, not three.
