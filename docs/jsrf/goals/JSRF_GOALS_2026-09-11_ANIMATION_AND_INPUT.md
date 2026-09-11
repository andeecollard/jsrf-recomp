# Goals — 11 Sep 2026: id 45's animation, and the genuine input stop

Supersedes the plan sections of `handovers/CLAUDE_HANDOVER_2026-09-10_ANIMATION.txt`
(§18, §25) for sequencing. The rig those steps run on is documented in
`plans/RIG_SETUP.txt`.

Two faults are open and they are independent. Neither is blocked on the other.
What *is* blocking both is a build question, so that comes first.

---

## 0. Decide `ff5f578` (the MCPX writable alias) — blocking

**State.** Committed today on `reference/xemu-oracle` as "Keep MCPX guest writes
guarded through a writable alias", validated as "the SIGSEGV is pre-existing".
It is not: 9 faults at `0x3FFFFFFBE` across 18 alias runs, and 0 occurrences in
72 non-alias runs over two days. A controlled A/B today (same build dir, same
gen, same env, only `git checkout ff5f578^ -- src/kernel/xbox_memory_layout.c`
between arms) gave 1/3 faults with the alias under the sampler harness against
0/5 without it, and 0/3 with the alias under plain bounded runs.

**Why it blocks.** A build that dies at ~10 s in a meaningful fraction of runs
corrupts every differential in threads A and B. Nothing in either thread needs
the alias.

**Do one of:**

1. **Revert it** (recommended) and reland once understood. One commit, and the
   rig returns to the configuration all 72 clean runs came from.
2. **Root-cause it first**, if it is wanted sooner. The cheapest split is to
   bisect the change into its two halves, since they are independent:
   - the *mapping* half — the double `MapViewOfFileEx`/`MapViewOfFile` of the
     8 MB aperture;
   - the *handler* half — trapped stores written through `g_mcpx_regs` and the
     early return in `mcpx_trap_handler`.

   Build with the mapping in place but the handler routed the old way. If the
   fault survives, it is the mapping; if it disappears, it is the write-through.
   The fault itself is guest VA `0xFFFFFFBE` with `EAX=FFFFFFB4` (-76) and last
   instrumented function `sub_00147EBB` — an error value used as a pointer, so
   the real question is which call started returning -76.

**Do not** carry the alias under a thread A or B measurement until this is
settled.

---

## Thread A — id 45's missing animation writes

The objective. Measured like-for-like at the Corn tutorial, xemu writes 449
dwords on CPlayer id 45 in 10 s and we write 137; the 330 we never write are
structured (`+0xCE0..+0xDE4` 66 dwords, `+0x12C0..+0x140C` 4×15, plus 3-dword
vector groups) and read as bone matrices. id 44 is near-static in xemu too, by
design — it is not Corn, and half the earlier investigation was measuring
correct behaviour.

### A1. Exec-walker counts at the tutorial *(one human run; ~10 min)*

The only outstanding measurement on the mode question. Arm 12 counters on the
five walkers plus `CActMan::ActionExec` and the `drawOne` control:

```
0x00011070 recursiveExec0Default      0x00011D00 recursiveExec1Default
0x000112A0 recursiveExec0Event        0x000114D0 recursiveExec0CoveredPause
0x00011700 recursiveExec0FreezeCam    0x00011930 recursiveExec0UncoveredPause
0x000123E0 CActMan::ActionExec
```

Gameplay runs `Exec0Default` 33,916 times with every other walker at zero, and
the new-game menu matches. The tutorial's mode booleans (`CActMan +0x38..+0x48`)
are all-zero and byte-identical to xemu.

- **Default only** → mode selection is exonerated outright; the fault is inside
  the Default exec method for id 45, and §21's mechanism is dead.
- **Anything else fires** → the wrong slot runs despite identical flags, and the
  decompilation video's own example is our bug.

Run it with `RECOMP_SCENE_REPORT=1` so the log says whether the run was paused.

### A2. Find the writer of `+0xCE0..+0xDE4` *(unattended half + one human run)*

Same binary both halves — gameplay writes the block, the tutorial does not — so
whatever runs in one and not the other is the target.

- Keep the probe set **light**. 545 sites ran clean; 578 in the XDK band froze a
  run at 253 polls. Stay near 150 and inside the game-logic range.
- Use `instrument_func_hit.py --ecxpair VA:0` so each hit records `(this,
  vtable)` and CPlayer calls are identifiable without a dump.
- Static grep will not find this writer: 66 contiguous dwords plus 15-dword
  groups is a block copy or an indexed loop, not `mov [reg+disp]` stores. The
  four functions from the earlier narrow differential write ~14 offsets between
  them and cannot account for it.

### A3. If A2 comes back empty — sample, don't probe *(unattended)*

The block *is* written in our own gameplay, so a sampling differential works
without any instrumentation: profile in gameplay and at the tutorial and diff
the guest frame sets, exactly as the pad-poll sampler does. Zero per-call cost,
and it sees block copies that probes and watchpoints both miss. This has not
been tried and is cheap.

---

## Thread B — the genuine input stop *(all unattended)*

Two phenomena were conflated; `[JSRF-PAUSE]` now separates them in one line.
Pause (`+3C/+40 = 1`, `Exec0CoveredPause` running) is a synthetic START press
and is not a bug. The genuine defect is `polls` flat or 0 while **not** paused,
with `Exec0Default`/`Exec1Default` still running normally.

### B1. Frame-set differential — started today, not yet conclusive

`sample_pad_poll.py` captured two genuine pairs (unpaused, polls flat at 1,864
and 3,751). Frames present when healthy and absent when stalled, intersected
across both runs: **21 functions, every one at 1–4 samples**, and nothing in the
USB path. That is noise-level.

Next: longer samples (`--seconds 10`) and four or five pairs, then intersect.
A frame that matters should survive every pair. Also worth capturing: the
`polls = 0` variant, where the guest never polls at all yet still loads a scene —
that may be a different failure from "polls stop after N".

### B2. Classify the real-pad flatlines *(free)*

Four of ~14 real-pad runs flatlined before `[JSRF-PAUSE]` existed, so none was
checked for pause state. The next one self-diagnoses. No new tooling.

### B3. What is already dead — do not re-open

`ohci_raise()` is never called at all, in healthy *and* stalled runs (0 raises,
0 drops, positive control taken). The interrupt worker never reaches its
operational state in any run. The level-trigger change was written, tested and
reverted. The interrupt path is unused end to end; the stop is the guest
ceasing to drive the schedule.

---

## Infrastructure — the actual bottleneck

Roughly twenty-five human playthroughs have gone into this investigation. That,
not compute, is what the schedule is made of.

- **C1. xemu save states** via `-monitor` + `savevm`. Turns "play to the Corn
  tutorial" from a ten-minute human task into seconds, and makes every oracle
  measurement repeatable. Highest-value item on this list.
- **C2. Scripted pad input past the menus**, so the tutorial half of A2 stops
  needing a person. `RECOMP_FAKE_PAD`'s START window is the primitive; what is
  missing is a scripted sequence rather than a metronome.
- **C3. Mine the log corpus before running anything.** 90 preserved runs under
  `render-investigation/`. Today's alias result came entirely out of it, with no
  new runs needed to establish the association.

---

## Housekeeping

- `reference/xemu-oracle` is **local only** and six commits ahead of
  `origin/main`. Everything this week exists on one disk in an iCloud folder.
  Decide whether to push it; if the log's prose convention matters, `9a114fe`
  ("Integrate upstream Xbox recompilation changes") is one generic commit
  sweeping together the morning's instrumentation and older integration work,
  and wants splitting first.
- `ctest` is **21/27** on this gen. Five failures are backport-presence checks
  against a gen regenerated without them (their `_semantics` siblings pass) and
  one is the known 107-vs-76 dead-`_flags` ratchet. Either re-apply the
  backports to this gen or move the ratchet deliberately — but stop reading
  "27/27" from CLAUDE.md as the baseline for this tree.
- `docs/jsrf/handovers/CLAUDE_TO_CODEX_HANDOVER_2026-09-11_ANIMATION_AND_INPUT.txt`
  and `diagnostics/jsrf_first_fault/sample_pad_poll.py` are untracked. Commit
  them so the next session finds them in git rather than by path.
