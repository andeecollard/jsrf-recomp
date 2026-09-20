# State-driven Mac harness: startup to tutorial entry

20 September 2026. User requested a new harness for both startup/tutorial and
later every level/mission, starting with startup → tutorial. Implemented the
first bounded scenario; no claim that all stages or tutorial objectives are done.

## Changes

- `diagnostics/jsrf_first_fault/main.c`: 14 lines connect the optional stage
  bridge to the USB-report shim and start its observer just before guest entry.
- `diagnostics/jsrf_first_fault/stage_harness/`: control bridge, Python runner,
  JSON startup scenario, README and tests. No edits to generated code, renderer,
  game assets, normal player configuration, existing replay scripts or saves.
- Separate build at `/tmp/jsrf-stage-harness-build/jsrf_first_fault`. Existing
  generated tree reused; no regeneration or app deployment.

The controller uses short expiring input leases checked against the live game
sequence at delivery. If the scene changes, a queued START cannot turn into
PAUSE. The observer checks the sequence table and CPlayer identities, reports
missing readings explicitly, and publishes a heartbeat. Python requires both
fresh observation and advancing guest frames. HDD copies are APFS clones where
available, verified by SHA-256 manifests that include the loose UDATA tree and
empty directories. The source HDD is untouched.

See the module README for exact commands, protocol, extension points and limits.

## Live evidence

Game: configured retail dump under `~/Library/Application Support/JSRF/game`.
Source HDD: `~/jsrf-build/emulated-hdd-warm`. Each run staged a new independent
copy. No inherited RECOMP/SDL switches; audio device live, PAD_INJECT off.

Binary SHA-256:
`446e3075625d609fc691cde7a2357098ec10a7546acfb46523bdc5e2010ffdd6`

| Run directory | Outcome | Total | Setup | Interpretation |
|---|---|---:|---:|---|
| `/tmp/jsrf-stage-run-01` | failed | 38.69s | 6.37s | Reached Corn's opening dialogue, but driver incorrectly timed out waiting for title readiness after the game had already advanced. Fixed in navigate(), covered by a regression. |
| `/tmp/jsrf-stage-run-02` | review_required | 35.76s | 6.22s | All configured entry gates reached; final screenshot inspected: opening Garage dialogue, two CPlayers. |
| `/tmp/jsrf-stage-run-03` | review_required | 35.84s | 6.22s | Same configured gates reached again, sequence 30, two CPlayers. |

Runs 02/03 used the corrected Python runner against the same binary. Sample
counts/frame counts differ; this is repeatable navigation in two trials, NOT
proof of deterministic simulation. The first run is retained as a failed run,
not counted as a passing harness trial.

The explicit machine verdict remains review_required (exit 3). Sequence 30
means a mission, not specifically the tutorial; two players can exist in other
contexts. The screenshots identify the reached scene in these runs. There is
no automated tutorial-identity, objective-completion or controlled-movement
oracle yet, and the harness does not claim one.

Each run contains manifest.json, hdd-manifest.json, result.json, runtime.log,
events.jsonl and picture-N.bmp. Converted final.png images are also present.
Setup accounts for ~6.2s; the successful runs cost ~29.6s after setup, including
capture and orderly shutdown. No runtime performance speedup is claimed.

## Validation

- Separate full Mac build passed (existing warnings, including link alignment).
- Seven Python tests passed: invalid/missing state, frozen frames, observer
  failure, variable loading time, the advancing-during-readiness regression,
  fixture content/directory changes and honest initial scenario verdict.
- Standalone C negative controls passed against the actual bridge functions:
  inactive-by-default, valid lease delivery, changed sequence suppresses START,
  expired lease releases input.
- `git diff --check` passed. No broad-suite result is claimed.

## Next boundaries

1. Title/menu internal readiness and selected menu item. Current sequence-12
   START/A groups are bounded and stage-conditioned, but spacing is heuristic.
2. Tutorial dialogue/objective state, then controlled movement/jump validation.
   The sampled +0xCE0 transform also changes in a cutscene; do not score that as
   proof an input was consumed. Hook delivery counts are not guest consumption.
3. Verified save fixtures for each desired mission/level, loaded by normal game
   routes, plus continuous progression tests to catch transition bugs.
4. Per-scene graphics/audio/performance assertions only after identity matches.

Full machine snapshots and arbitrary sequence-index writes are not implemented.
A save-based start is not a whole-runtime restore.

## User question: does JSRF have a debug mode?

No developer debug menu was established. `PrepareTestRun` / `WaitEndTestRun`
are not evidence for one: Test Run is the normal game feature documented in
the original manual. An unlocked save may make that mode useful for later level
scenarios. Search results for the original Dreamcast Jet Set Radio's prototype
debug menu must not be attributed to Xbox JSRF.
