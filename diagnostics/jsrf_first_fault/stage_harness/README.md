# State-driven Mac stage harness

This is the first slice of the startup → tutorial → individual level/mission
harness. It drives the existing runtime through its USB controller-report hook;
it does not replay a recording or change game sequence numbers. It is opt-in and
leaves ordinary launches and the old replay scripts untouched.

## Current coverage

`startup.json` boots, observes the title, attempts bounded START/A navigation,
and stops on sequence 30 or 34 with **both** registered CPlayer objects validated.
The first live captures show Corn's opening Garage/tutorial dialogue. Sequence
30 is shared with other missions, so the machine verdict is deliberately
`review_required` (exit 3), not “tutorial passed.” The scenario does not yet
complete the jump, grind or graffiti objectives, or establish controlled movement.

The title's internal menu readiness is not identified yet. START/A attempts are
conditioned on sequence 12, separated by releases and bounded in number/time;
their within-menu spacing remains a heuristic. This tolerates variable boot
length but is **not deterministic guest scheduling**. Each attempt is recorded.

## Build separately, without regenerating

From the repository root (choose a writable build location):

```sh
cmake -S diagnostics/jsrf_first_fault -B /tmp/jsrf-stage-harness-build \
  -DRECOMP_GEN_DIR="$PWD/build-macos/jsrf-first-fault/gen" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/jsrf-stage-harness-build --target jsrf_first_fault -j 6
```

This changes only `main.c` plus the included `bridge.h`. It neither installs a
new player app nor replaces a preserved baseline. Current binary for the first
validation runs: `/tmp/jsrf-stage-harness-build/jsrf_first_fault`.

## Run

```sh
python3 diagnostics/jsrf_first_fault/stage_harness/run.py \
  --binary /tmp/jsrf-stage-harness-build/jsrf_first_fault \
  --game "/path/to/game-directory-containing-default.xbe" \
  --hdd "/path/to/preserved-emulated-hdd" \
  --out "/path/to/new-run-directory"
```

A new output path is required. Process enumeration must work, and an existing
`jsrf_first_fault` process prevents launching. GPU/window access is required on
macOS. The runner terminates only the child it launches, initially with SIGTERM.
Ctrl-C also releases input and stops that child.

The source HDD is never written. The runner hashes it, makes an independent APFS
clone where supported (ordinary copy otherwise), hashes the copy, and requires
identical file contents **and directory entries**. Saves remain in the private
copy. Setup time is reported separately in `manifest.json`; the verified source
contains the loose UDATA save tree as well as partition images and warm cache.

The environment removes inherited `RECOMP_*`, `JSRF_*` and `SDL_*` variables.
Audio stays live by default. Runtime defaults apply; this is not automatically
the player's custom performance configuration. Explicit experiments can use
`--env RECOMP_NAME=value`; every effective override is recorded. Pad replay,
fake-pad, sentinel and pad recording modes are incompatible and refused.
`--env RECOMP_PAD_INJECT=1` is possible for a labelled diagnostic bypass, but the
initial successful runs used the USB path with injection **off**.

## Evidence and verdicts

- `manifest.json`: binary and XBE SHA-256, full scenario, runtime configuration,
  HDD source and setup duration. Binary identity is checked again at completion.
- `hdd-manifest.json`: exact initial copied HDD contents, including empty dirs.
- `events.jsonl`: sampled state, every command, input-hook acknowledgements,
  navigation attempts and step results.
- `runtime.log`: the runtime's normal diagnostics, build/generation banner,
  faults, audio and renderer reports.
- `picture-N.bmp`: best-effort snapshots at successful steps and final failure.
  These use the existing presented-frame snapshot without an added GPU sync;
  they can tear and are supporting evidence, not an atomic state oracle.
- `result.json`: explicit outcome and last observed state. Exit 0 means the
  scenario's stated gates passed, 1 a failed run, 2 a preflight/configuration
  error and 3 a reached scenario requiring review. No auto-retry hides failures.

A live observer alone cannot pass a gate: frames must also advance. Missing
observer heartbeat fails after five seconds. An unreadable scene or unmatched
64-entry function table is unknown, not success. Player data requires the known
CPlayer vtable, own object id, bounded address, finite position and a rechecked
registry pointer. The +0xCE0 transform entry is existing diagnostic knowledge;
its changes alone do not prove input-driven movement (cutscenes also move it).
Objects are sampled without pausing guest threads, so samples are observations,
not coherent whole-machine snapshots.

## Control protocol v1

`JSRF_STAGE_DIR` enables a 20 Hz observer thread. It atomically publishes
`status.json` and reads an atomically replaced `command.txt`:

```
id ttl_ms expected_sequence buttons A B RT left_x left_y snapshot
```

IDs increase. A command is accepted once. TTL is capped at 1000 ms; repeats of
the same file do not renew it. The input hook returns a neutral virtual port 0
unless the lease is live **and the current sequence equals the expected one**.
The controller remains neutral after runner death, never reverting to a live
pad or old script. State is mutex-protected. `deliveries` means the USB-report
hook supplied that command; it does not prove the title sampled or acted on it.
The optional existing injection bypass also goes through this hook and is
explicitly reported as `input_injection`.

`status.json` includes protocol version, monotonically increasing sample,
guest frame, sequence, checked-table flag, command ID, delivery count, lease
status and players 44/45. The bridge is inactive without `JSRF_STAGE_DIR` and
is excluded from Windows builds.

## Tests

```sh
python3 -m unittest discover -s diagnostics/jsrf_first_fault/stage_harness -p 'test_*.py' -v
cc -std=c11 -D_POSIX_C_SOURCE=200809L -pthread \
  diagnostics/jsrf_first_fault/stage_harness/bridge_test.c \
  -o /tmp/jsrf-stage-bridge-test
/tmp/jsrf-stage-bridge-test
```

The negative controls exercise the actual C lease/state guard and Python gates:
expired commands, changed sequence, absent player, invalid position, frozen
frames and missing observer. A regression covers the game advancing while the
runner was waiting to send its next press.

## Driving the game: what is measured, and what it cannot do

`tutorial.json` gets from the title to an interaction with the other character
in the Garage without a recording, a save or a frame number. Six of its seven
steps pass; the seventh is the current frontier and is described below.

Three facts about JSRF's control were measured on 20 September 2026 and the
navigation is built on them. Each is an observation from a live session, not a
reading of the game's code:

- **The camera chases the character**, so "up" on the stick means *keep the
  heading you already have*. Five consecutive forward legs converged on one
  world heading (0.00, -1.00) while the distance covered rose 8 -> 55 units a
  leg. A stick angle is therefore a turn relative to the current heading, and
  a stored stick-to-world table is stale the moment it is written.
- **The stick is analog over its whole range.** The bridge has always validated
  `left_x`/`left_y` across the full signed 16-bit range; only the `BUTTONS`
  table in `run.py` reduced that to four presets. Steering needs the angles
  between them, and it needs the magnitudes too: full deflection is a skate
  with a turning circle, a small one is a walk. Runs 07 and 09 orbited the
  target at radii of 20 and 40 units, always at full stick.
- **Handedness is measured, never assumed.** Which way a positive world turn
  maps onto the stick is calibrated at run time from a leg that asked for a
  turn big enough to show it, and is re-checked once. `--sense` overrides it
  for an experiment.

**What navigation does not do: pathfinding.** It steers in a straight line at
the target. Walking from Corn to the other character works; walking back to the
spawn point failed every time in two sessions, because the way back is round
Garage geometry. A `stalled` outcome with a position is the useful report here,
not a failure to be retried.

**Accuracy is about 15 units, not 5.** The character skates, carries about 8
units after the stick is released, and cannot stop dead. Tolerances below ~15
were not reached in any trial. The interaction in the Garage opens at ~12-20
units, so this is enough to talk to somebody and not enough for anything that
needs to be stood on.

### Step kinds

- `wait` — hold until a scene condition is stable, as before.
- `navigate` — bounded button attempts in a menu, as before.
- `motion` — the neutral-baseline movement check, as before.
- `talk` — press a button until a `manager.*`/`players.*` field reaches a
  value, recording every dialogue span seen. An `opener` is delivered exactly
  once first: **the button that opens an interaction is not the button that
  advances it.** Pressing RT repeatedly beside the other character flipped it
  between states 24 and 25 and showed one dialogue span that vanished again --
  each press cancelled what the one before it started.
- `goto` — walk player 44 to a world (x, z) or to another player's live
  position. Reports `arrived`, `stalled`, `scene_changed`, `deadline`,
  `unreadable` or `no_input`, always with the whole track.

`accept` lists the outcomes a step treats as progress. A stall is reported with
where it stopped and how close it ever got; it never passes.

### What the tutorial run established about the game

| observation | evidence |
|---|---|
| the text node at `+0x54` is the span **currently being typed** | it grows character by character: `'This is '` -> `"This is the GG's Garage.$nHey, where's our pizza?"` |
| dialogue carries layout markup | `$x320$n$x296Corn` -- `$x` is an absolute x position, `$n` a newline |
| player state 23 is a scripted cutscene; state 2 is a player-opened interaction | Corn's introduction ran at 23; RT beside the other character gave 2 |
| `A` advances a cutscene but is **jump** outside one | 20 A presses outside the cutscene only flipped state 1<->2 |
| the objective marker moves -1/-1 -> 1/2 when the introduction ends | `manager.progress_7930`/`_7934`, every run |

### The frontier

After RT the game shows `"Why don't you talk to her now? "` and the objective
marker does not advance. Two candidates were on the table: the approach is not
close enough, or the other registered player is not the character the objective
means.

**The first was tested and is unlikely.** Three navigations in one boot, at
tolerances of 10, 6 and 6, closed to 10.9, 11.5 and 16.8 units and stalled.
About 11 units is this controller's floor, the interaction hint already fires
there, and closing further needs the character to stop rather than a better
heading.

**So the second is where to look, and it cannot be checked yet.** The bridge
reports players **44 and 45 only**, while a street capture held three objects
carrying the CPlayer vtable. Enumerating those objects instead of hardcoding
two ids is the next change. It is a bridge change, so unlike everything else
here it needs a rebuild.

## Extending coverage

1. Identify the title menu's inner readiness/selection fields and tutorial's
   objective/dialogue state. Replace heuristic menu attempts with those gates.
2. Add a short neutral baseline and controlled movement/jump scenarios. Compare
   the same player object, position and objective counters; animation is not a
   movement acknowledgement.
3. Add verified save fixtures per chapter/mission, each with its own expected
   level and objective identity. Load through the game's normal loading route.
4. Keep a separate continuous progression suite to catch transition bugs that
   starting from saves could hide. Add graffiti, audio and pacing checks locally
   to each scenario after proving scene equivalence.

Full savestates are not implemented. Guest RAM alone would omit host threads,
GPU work, audio and file handles. Arbitrary writes to the sequence index are
not a valid substitute for loading a level.

`PrepareTestRun` / `WaitEndTestRun` refer to the game's normal Test Run feature;
they are not evidence of a hidden developer debug menu. An appropriately
unlocked save may make that normal mode useful for later level scenarios.
