# The harness drives the game, 20 September 2026 (evening)

Continues `CODEX_HANDOVER_2026-09-20_STATE_DRIVEN_HARNESS.md`, which took the
stage harness to the tutorial scene and stopped there. The player asked to
"move through and debug the stages of the game". This is how far that got, what
it measured on the way, and the one change that needs a rebuild.

**No C changed. No regeneration. No new binary.** Everything below runs against
Codex's `/tmp/jsrf-stage-harness-build/jsrf_first_fault`, sha256
`446e3075…0ffdd6`, verified by content to contain the text reader before use.

## What the harness can do now

`tutorial.json`: title -> Garage -> dismiss Corn's introduction to the first
objective marker -> **walk to the other character** -> **open an interaction
there with RT**. Six of seven steps pass, twice, from a cold boot in about
70 seconds with no recording, no save and no frame number.

Two new scenario step kinds, both pure Python:

- `goto` — walk player 44 to a world (x, z) or another player's live position.
- `talk` — press a button until a `manager.*`/`players.*` field reaches a value,
  recording every dialogue span, with an `opener` delivered exactly once first.

And one fix that matters more than either: **the debugger window now opens
after a FAILED step**, not only after a passing run. That is when the game is
standing at the boundary worth inspecting. Before this change, learning why a
step failed cost a whole boot; the three-trial approach measurement below was
taken through it, immediately after the step that motivated it.

## The enabling discovery: the stick was never four buttons

The bridge has always validated `left_x`/`left_y` across the full signed 16-bit
range. Only the `BUTTONS` table in `run.py` reduced that to four presets. So
analog steering needed **no C change and no rebuild** — the protocol already
carried it. `Driver.command` takes an explicit pad vector now and checks it
against the bridge's own bounds, because a command the bridge drops reads
exactly like a dead lease.

## What was measured about JSRF's control

Each of these is a live observation, not a reading of the game's code. Each
also cost a wrong turn first, which is why they are written down.

1. **The camera chases the character.** Five consecutive forward legs converged
   on one world heading (0.00, -1.00) while distance per leg rose 8 -> 55 units.
   "Up" therefore means *keep the heading you have*, a stick angle is a turn
   relative to the current heading, and a stored stick-to-world table is stale
   the moment it is written. The first controller stored one and thrashed.
2. **Magnitude is speed, and speed is a turning circle.** At full deflection
   the character orbited the target at radii of 20 and then 40 units without
   ever converging. The stick is now cut for either reason a player would ease
   off — the target is close, or the turn is sharp.
3. **Handedness is measured, never assumed**, from a leg that asked for a turn
   big enough to show it, and re-checked once. Latching on the first marginal
   leg sent four of eight trials the wrong way.
4. **Turning round is not stalling.** Every return trip was called stalled while
   the character was still coming about. A leg now counts as progress if it
   closed distance *or* improved the heading.
5. **There is no pathfinding, and the walk is line-of-sight.** Corn -> the other
   character works every time. The reverse failed in every trial in two
   sessions: the way back is round Garage geometry. `stalled` reports where it
   stopped and how close it ever got; it never passes.

## What was measured about the game

| observation | evidence |
|---|---|
| the text node at `+0x54` is the span **currently being typed** | it grows: `'This is '` -> `"This is the GG's Garage.$nHey, where's our pizza?"` |
| dialogue carries layout markup | `$x320$n$x296Corn` — `$x` an absolute x position, `$n` a newline |
| player state 23 is a scripted cutscene; state 2 is a player-opened interaction | Corn's introduction ran at 23; RT beside the other character gave 2 |
| `A` advances a cutscene and is **jump** outside one | 20 A presses outside the cutscene only flipped state 1<->2 |
| the objective marker moves -1/-1 -> 1/2 when the introduction ends | `manager.progress_7930`/`_7934`, every run |
| the button that OPENS an interaction is not the one that advances it | repeated RT flipped the other player 24<->25 and showed one span that vanished again: each press cancelled the one before |

**The text reader's first live execution was this session.** Codex added it at
15:27, after the only session that could have run it, so `movement.json`'s
`text_contains` gate had never executed. It works, and it is the instrument G2
has been missing: the goals file records that the string in guest memory is
whole and the defect is *which span is drawn*. This reads the span the game
believes it is drawing, at 2 Hz, beside a screenshot taken under the same
command id.

## The frontier, and what to do next

After RT the game shows `"Why don't you talk to her now? "` and the objective
marker does not advance. Two candidates:

- **the approach is not close enough** — tested, and unlikely. Three
  navigations in one boot at tolerances 10, 6 and 6 closed to 10.9, 11.5 and
  16.8 units. About 11 units is the floor; the hint already fires there.
- **the other registered player is not the character the objective means** —
  untestable today. The bridge reports players **44 and 45 only**, and a street
  capture held three objects carrying the CPlayer vtable (`0x001CCFF8`).

**So the next change is to enumerate the CPlayer objects rather than hardcode
two ids.** It is the one thing here that touches `bridge.h` and therefore needs
a rebuild. Everything else this session was Python against an unchanged binary.

## Evidence

`/tmp/jsrf-stage-run-{04..15}` and `/tmp/jsrf-garage-{01..03}`, each with
`manifest.json`, `hdd-manifest.json`, `events.jsonl`, `runtime.log`,
`result.json`, `report.md` and captures. Runs 13, 14 and 15 are the current
scenario; 15 is the confirmation, and its debug window was served after the
failing step.

Audio was off by request in every run (`SDL_AUDIODRIVER=no_such_driver`), so
**every audio counter in these logs is void by construction**. Nothing here
measures sound, frame time or rendering; the harness reached the scene, it did
not score it.

Runs differ from each other. Two boots are not comparable — that is why the
approach measurement was taken as three trials inside one boot, and why the
controller tuning that was done across boots earlier in the session produced
four contradictory results before the method was corrected.
