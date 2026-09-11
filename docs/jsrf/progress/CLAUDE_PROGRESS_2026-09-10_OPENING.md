# G1 — the card machine, measured, and where the fade cannot be

Date: 2026-09-10 (Europe/London)
Branch: `reference/xemu-oracle`
Build: working gen + `instrument_render_state.py`, 45 s run, `rstate2`

Follows `CLAUDE_PROGRESS_2026-09-10_INTRO_FADE.md`, which ended asking **what
computes the combiner constant**. This narrows that considerably and rules one
whole object out, but does **not** find the fade. Read §5 before continuing.

---

## 1. The instrument, and its positive control

Two probes, installed by one script through one mechanism into an existing gen
(no regeneration), removable with `--remove`:

| site | function | result |
|---|---|---|
| `0x001504D0` | `CMGameGL::setRenderState` | **never called** |
| `0x0007E360` | `Opening::Exec0Default` | 2415 calls |
| `0x0007E550` | `Opening::drawDefault` | 4829 calls |

**The pairing is the point.** `setRenderState` reporting zero across a run that
drew 102000 batches and rasterised 8.35 M triangles means nothing on its own --
it reads identically whether the guest never calls it or the probe is dead. It
means something *because the Opening probe, installed the same way in the same
run, reports 7244 times*.

So the negative is real: **`CMGameGL` is not the retail render path.**
`CMGameGLFont::draw(this, x, y, str)` sits beside it in the symbol table, which
reads as a debug overlay. Anything hoping to catch render state through that
class is aiming at dead code.

---

## 2. The card state machine

`Opening` runs 16 states. `drawDefault` switches on `card / 3` through a
5-entry jump table, so that is **5 visible cards of 3 states each**, plus state
15 falling through to the default case.

```
[OPENING] t=   0.04 card=0   timer=0     ticks=1     draws=0
[OPENING] t=   1.48 card=1   timer=0     ticks=121   draws=241
[OPENING] t=   3.00 card=2   timer=121   ticks=242   draws=483
[OPENING] t=   4.48 card=3   timer=121   ticks=363   draws=725
[OPENING] t=   5.95 card=4   timer=0     ticks=484   draws=967
...
[OPENING] t=  19.25 card=13  timer=0     ticks=1573  draws=3145
[OPENING] t=  29.14 card=14  timer=721   ticks=2294  draws=4587
[OPENING] t=  30.78 card=15  timer=721   ticks=2415  draws=4829
```

- **121 ticks per state**, ~1.45 s, so ~4.35 s per visible card.
- That lines up with the reference timeline already recorded: white logo
  t=4.4–8.4 s is card group 1 (states 3–5, measured 4.48–8.81), grey Dolby
  t=12.6–16.8 s is group 3 (states 9–11, measured 13.05–17.47).
- **State 13→14 is the outlier**: 9.89 s and `timer=721`, against ~1.45 s and
  121 everywhere else. Six times longer. Not explained here.

### draws = exactly 2 × ticks

Measured 1.992 → 1.999 across every state, converging on 2.0. This is **not an
anomaly**: `CActBase::drawTreeDefault1` and `drawTreeDefault2` are two separate
draw-tree walks, so every object is drawn twice per tick by design. Worth
recording because a 2× draw count is exactly the kind of number that gets
mistaken for a bug, and because the goals document flags our intro presenting
~58 fps against the reference's ~30 as an unexplained divergence -- a factor of
two that now has a candidate explanation on the draw side.

---

## 3. Where the fade is not

Every field of `Opening`, read from the disassembly rather than guessed:

| field | what writes it | what it is |
|---|---|---|
| `+0x98` | `Exec0Default` | card index, 0–15 |
| `+0x9c` | `Exec0Default` | per-state timer, 0–121 |
| `+0xa0` | `Exec0Default` | skip flag, set when any of the four pads has a button down |
| `+0xa4` | constructor | zeroed |
| `+0xa8` | constructor, once | `FileGet()` handle — consumed by `UnknownStatic25::getTexIndex(uint key)`, so a texture key |
| `+0xac` | constructor, once | `FileGet()` handle |
| `+0xb0` | constructor, once | `Language::Get()` |
| `+0xb4` | constructor, once | `CSaveData::GetReturnChapterNo()` |
| `+0xb8` | constructor | a sub-object |

**`Exec0Default` never writes `+0xa8` through `+0xb8`** — the whole tick, from
`0x0007E360` to `0x0007E550`, contains no reference to those offsets. They are
set once at construction and only ever read.

**And `drawDefault` never reads `+0x9c` or `+0xa0`** — zero references across
its entire range. The only per-frame quantity the object owns is the timer, and
the timer does not reach the draw path.

So `Opening` cannot produce a per-frame ramp. It holds a card index, a timer, a
skip flag, two file handles, a language id and a chapter number. The earlier
note's "the title never computes a fade level" is now not just an observation
about the combiner factor but a structural fact about this object.

The constants `drawDefault` does pass are fixed: `0xF0` to a renderer vtable
slot, and `2.0f` / `1.0f` to the `+0xb8` sub-object.

---

## 4. Ruled out — do not re-derive

| hypothesis | killed by |
|---|---|
| the fade rides on `Opening`'s state machine | `drawDefault` never reads the timer; 0 references |
| `Opening` computes a level into its own fields | `Exec0Default` never writes `+0xa8`..`+0xb8`; set once in the ctor |
| render state can be caught at `CMGameGL::setRenderState` | never called in 45 s / 102000 draws, against a positive control that fired 7244 times |
| the 2× intro draw count is a bug | two draw-tree walks by design (`drawTreeDefault1` / `2`), ratio 1.999 |

---

## 5. Still open, and the next move

**The fade level's source is not found.** What is now known is that it is not in
`Opening`, and not reachable through `CMGameGL`.

The remaining candidates, in the order worth taking:

1. **The `+0xb8` sub-object.** Constructed by `Opening`, called with two floats
   per draw. It is the only part of the card path not yet read.
2. **A separate fader object.** The reference's ramp is `0xAARRGGBB` with the
   alpha byte moving while `a0ff60` holds — a level times a fixed tint. That
   shape suggests a general-purpose fader drawing over the cards, not the card
   object itself.
3. **The XDK's own state table.** `CMGameGL::setRenderState` dispatches through
   a table at `0x1C4248` indexed by state, into 30 handlers at
   `0x0018E8A0`–`0x0018FA30`. Those handlers are the real emitters and are
   reached from the retail path by some other caller. Probing one of them and
   reading the return address would name that caller, which is the same trick
   that worked here.

**Do not widen the renderer's accept set to chase this.** Measured previously
and again consistent with this run: the guest asks for one blend combination
throughout the cards.

## 6. Reproducing

```sh
python3 diagnostics/jsrf_first_fault/instrument_render_state.py     # install
cmake --build build-macos/jsrf-first-fault/build -j
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_OPENING_TRACE=1 RECOMP_RSTATE_TRACE=1 \
RECOMP_HDD_ROOT=<scratch> python3 diagnostics/jsrf_first_fault/run_bounded.py \
    rstate2 --seconds 45
python3 diagnostics/jsrf_first_fault/instrument_render_state.py --remove
```

45 s covers all 16 states. Pipe any log through `symbolize.py annotate` to turn
the raw addresses into names.
