# JSRF goals — every edge is counted, 19 September 2026 (evening)

Supersedes `JSRF_GOALS_2026-09-19_ONE_ROOT_CAUSE_UNDER_TWO_ITEMS.md`. That
file keeps the full evidence for G20 (the switch arm) and G21 (the texture
path); this one records what happened to G20, adds **G22**, and resets the
order.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

G1–G21 carry forward. The control-flow completeness work takes **G22**.

## What changed since the midday file

**G20 is regenerated, built and played.** The merged tree was regenerated at
12:45 (gen `c4400dbb0025f56d`, head `2bf7c1d`), built at 14:13, and the
player ran it for 1209 seconds with zero `[ITAIL]` failures, no black frame,
no guest fault and no ADX freeze (`2cf2d9e`), against 307 seconds for the
morning's build. A pad recording of that session exists:

    ~/Library/Application Support/JSRF/padrec/graffiti-2026-09-19_1309.padrec   (+ .KEEP)

Its header reads `#!gen c4400dbb0025f56d`. **That is the translator hash**, and
it is the whole of what the replay check compares (`xinput_device.c:1427`,
stamped from `JSRF_GEN_TRANSLATOR`, which comes from the gen's manifest). Any
change under `tools/recomp` changes it at the next regeneration, and every
binary built on that regeneration refuses this recording.

**What the played session opened**, from the same commit and `c6ed54a`: the
text defect has a second symptom (variable leading-character loss, a line
flush against the window's left edge), the corruption is animated rather than
stable, the string in guest memory is whole (measured 29 times) so what is
wrong is *which span* of it we draw; and graffiti does not plant while the
sound fires and the can count falls. Those are G2's and G21's evidence and
stay in their files.

## THE ORDERING CONSTRAINT, restated for G22

G22b below changed `tools/recomp/lifter.py`. It is **not regenerated**, on
purpose. The current gen, the 14:13 binary and the 13:09 recording all still
agree on `c4400dbb0025f56d`.

The runtime half of G22b lives in `recomp_types.h` and `recomp_manual.c`,
which are compiled from the repo. The header was **hand-copied into the gen
tree** (the legitimate move `jsrf_gen_header_current`'s comment describes) so
that test stays green and the binary carries `recomp_unimpl` — which nothing
in the current gen calls yet. Two configure-time warnings now fire and are
expected until the next regeneration:

    STALE GENERATED TREE   -- tools/recomp changed (the lifter), gen did not
    STALE RUNTIME TYPES    -- the manifest's header sha predates the hand copy

**Do not regenerate to silence them.** Regenerate when the 13:09 recording has
been spent — replayed for what it was taken for, or the player decides a new
session is cheaper than the old one — and bundle every pending translator
change into that one regeneration, as the midday file's rule says.

## G22 — control-flow completeness: every edge accounted for, and gated

The midday file named two gates that worked, coverage and the unresolved-table
count, and both lived in prose. Neither was a test. A regeneration that
stranded a hundred more arms would have built green. And the lifter's answer
to an instruction it cannot translate was a bare C comment: the instruction
vanished, and nothing at runtime could say the site had even been reached.

### G22a — the gate — DONE

`diagnostics/jsrf_first_fault/control_flow_gate.py`, registered as
`jsrf_control_flow_gate`, classifies **every** transfer the gen tree emits and
holds each unresolved or silent count at or below
`control_flow_baseline.json`, coverage and resolved switches at or above it. A
shape it does not know fails the run (it found one on its first pass:
`RECOMP_ITAIL(MEM32(eax))`, a slot jump with no displacement, now classified).
Its switch-arm numbers reproduce `switch_arm_audit.py` exactly.

The baseline, on gen `c4400dbb0025f56d`:

| class | count | reading |
|---|---:|---|
| direct calls / direct tail jumps | 43,170 / 3,578 | translated |
| gotos, of which switch arms | 65,878 / 3,757 | translated |
| switches lifted to gotos | 487 | translated |
| indirect calls through the dispatch table | 7,418 | runtime-resolved |
| tail jumps through a slot / global / register | 291 / 13 / 13 | runtime-resolved |
| **unresolved switch dispatches / tables** | **152 / 62** | G20's shape |
| **switch arms within the window, with no body** | **571 / 487** | the damage |
| call targets that reach a stub | 168 | detector never defined them |
| **untranslated instructions, all silent** | **122 / 122** | G22b |
| conditional branches with no target / bare `jmp` with none | 0 / 0 | |
| functions / unique bytes covered | 8,846 / 1,613,483 | union of extents |

Accept a deliberate change with `--write-baseline`, in a commit that says why.
The gate cannot see a branch translated to the *wrong* place, only to nowhere,
and cannot say whether an edge is ever taken; those are G22c and G22d.

### G22b — no silent omission — DONE IN THE LIFTER, PENDING REGENERATION

Eleven exits in `lifter.py` returned `/* TODO: mnemonic */`; three recorded the
site in the translator's tally and eight did not. All eleven now go through
one helper that records the site and emits

    RECOMP_UNIMPL("mnemonic ops", 0xVAu); /* TODO: mnemonic ops */

`recomp_unimpl` logs the guest address, hit count and eight registers on the
ICALL cadence, and under **`RECOMP_UNIMPL_TRAP=1`** aborts at the first hit —
at the cause, not downstream. Default off; a healthy run reaches none of these
and prints nothing. Deliberately ignored instructions (`RECOMP-IGNORED-PRIV`,
cache hints) carry no marker; `tools/recomp/test_lifter_unimpl.py` pins both
sides. The 122 sites are `bound`, `arpl`, `hlt`, `outsb`, `sti`, `enter`,
`daa`… — what a linear sweep reads over data — so most are probably dead.
*Probably* is the word this goal exists to remove.

**Done when:** after the next regeneration `todo_silent` reads 0 in the gate,
and one scripted run with `RECOMP_UNIMPL_TRAP=1` either completes or names a
live site by address. Either answer is a result.

**The `popfd` question is closed, no run needed.** The lifter's own note at
`lifter.py:450` says every `popfd` in this title disassembles inside data —
the zero-padded regions of `DOLBY` (0x0027Exxx) and `DSOUND` (0x001Axxxx) —
and the four gen sites bear it out: each sits between an `ebx = 0xDE00129Bu`
and a `hlt`, i.e. a linear sweep reading bytes. Upstream #72 is about the
instruction's semantics, not these sites. `hlt` has 7 sites in the same
regions; the lifter deliberately refuses to no-op it because that would turn
a wait into a spin, and the marker will say whether any is ever reached.

### G22c — the runtime side, per run — TOOL DONE, corpus pass open

`diagnostics/jsrf_first_fault/edge_runtime_summary.py` reads a run's
`stderr.log`, takes every VA the runtime reported — `[ITAIL]` unresolved,
`[ICALL]` failed, not-code, runaway, `[UNIMPL]` reached — and says which
static class each is: an arm of which unresolved table (and its owner), a
stub, a body the gen defines (a dispatch-table miss, a different bug), an
untranslated site, or unknown to the detector. It prints the log's own
`[GEN]` line beside the gen's manifest, because a VA is only meaningful on
the gen that produced the log: run on the 08:25 crash log it correctly
reports that log as translator `b6f29140`, not the current gen, and
`0x000A5B8C` reads `unknown` there because on *this* gen it is no longer an
arm of anything — it lifts to a goto inside `sub_000A5B60`.

**Open:** run it over the 782-run corpus and the player logs, per gen, and
fold the reached-set into the handover. Until then the gate's 152 / 168 /
122 are upper bounds on what the title touches, not counts of what it does.

### G22d — the reference execution — OPEN, and bounded

There is no native 32-bit x86 on this machine and Rosetta cannot provide
one, so "compare guest and reference at the first divergence" cannot mean a
whole-program trace. What exists is `tools/conformance/fuzz_unicorn.py`:
register-only integer instructions against Unicorn, no memory, no FPU, no
SSE. Widening it to memory operands and the x87 stack is the only route to a
second opinion on translation *correctness* (as opposed to completeness), and
every adjudicated mismatch becomes a permanent case. Unicorn is a model, not
silicon; a disagreement is a lead.

### G22e — the 76 parked tables — OPEN, carried

Unchanged from the midday file: the remaining unresolved tables need a
*retraction* rule in the detector, and the thread to pull is
`_find_function_end`'s stopping rule on alias bodies. Route (b), loosening
the lifter's ≥2-arms rule, was measured unnecessary. Do not reopen it without
a number.

### G22f — explain the drop from 84 to 62 — HALF ANSWERED, and the method is now fixed

The midday file counted **84** unresolved tables, **607** arms and **8,866**
starts and quoted coverage **1,614,033**. The 12:45 regeneration reads **62 /
487 / 8,846 / 1,613,483**. The earlier numbers were taken on the shared gen
and on worktree trees that no longer exist, so they cannot be diffed. What
CAN be said, measured at 15:40:

**The detector is deterministic given its seed state, and one pass takes
17 seconds.** With `RECOMP_SEED_INTERIOR=1` — which `regenerate.sh` exports
by default and which turns on the pass that drops interior vtable seeds — a
single `tools.disasm` pass fed the final `vtable_seeds_accum.json` reproduces
the live `functions.json` **exactly**: 8,846 starts, 1,613,483 bytes, every
extent identical. Without that switch the same pass keeps 1,129 extra
`seed_vtable_thunk` starts (9,975), which is what a reader who runs the
detector by hand will see and should not mistake for a regression.

| pass | starts | covered | vs live |
|---|---:|---:|---|
| live 12:44 database | 8,846 | 1,613,483 | — |
| final accumulator + demotion (one pass) | 8,846 | 1,613,483 | **identical** |
| final accumulator, no demotion | 9,975 | 1,619,541 | +1,129 vtable-thunk starts |
| empty accumulator + demotion (the round-1 oracle) | 8,557 | 1,593,703 | 444 only here, 733 only live |

So the numbers drift only through the accumulator, and the accumulator is
built by the discovery loop from `func_id`'s vtable scanner — whose first
entry, `0x000207C2`, is arm 0 of the table at `0x000208E4`, the first
unresolved table in the gate's list. The scanner seeds jump-table arms as
functions; that was already known and is the reason the demotion pass exists.

Against the round-1 oracle the causes tool reads **54 of 62** tables
resolvable by the lifter's own ≥2-arms rule, 7 not-a-table, 1 with no body —
the same proportions as the midday 76 of 84. The eight that are not
resolvable even there are the real translation-quality backlog; the 54 are
extents carved from under the lifter, as before.

**Still open:** why this tree has 62 rather than 84. The likeliest reading is
that the midday 84 was measured on the pre-merge shared gen, whose
accumulator was built before the `wt-icall` short-table map existed, and the
merged discovery loop simply carves fewer arms; but that is a reading, not a
measurement, and it stays here as one. **What is fixed now is the method:**
every future number comes from the pass above, on a named gen, so the next
two counts will be comparable.

**A cheap gate this suggests:** a ctest that reruns that 17-second pass into
scratch and diffs it against the live `functions.json`. It would say
"the database on disk is not what the detector in the tree produces" — the
function-database analogue of `jsrf_gen_header_current`. Needs capstone on
the interpreter ctest uses; `/usr/bin/python3` has it, Homebrew's does not.

## G23 — the recording is the debugging case

Proposed by the player on 19 Sep evening: record inputs and starting
conditions, mark a problem while playing, replay unattended with diagnostics
around the mark, compare builds to the earliest execution difference, keep
the recording as a regression case. Measured against `xinput_device.c`:

| step | exists | missing |
|---|---|---|
| record inputs + conditions | frame-keyed on the guest's `FLIP_STALL`, analog exact, header carries build / gen / switch hash | the save and HDD state: the harness stages stock, the bundle uses the player's, nothing records which |
| mark a problem | nothing | a host key that writes `#!mark f<N> <label>` into the recording and `[PAD-MARK]` to the log |
| targeted diagnostics on replay | replay is the one mechanism; probes are per run | a frame window (`RECOMP_PROBE_FRAMES=a-b`, default ±300 around each mark) the dumpers and traces consult |
| earliest execution difference | per-checkpoint input hash AND guest-state anchor, reported separately | a per-frame state trace (esp at flip, icall count, draws, voices, seq state) beside the recording, and a diff that names the first frame two runs disagree |
| keep across regeneration | refused outright on `#!gen` mismatch, by design | **policy call, the player's:** with the trace, a cross-gen replay is a measurement of drift; accept with a banner and a first-drift verdict, refuse only on format version |
| delivery boundary | per frame; the guest polls at 120 Hz | key on the poll ordinal (the counter exists) so which of a frame's polls sees a press is fixed too |

**Order inside G23:** mark first (cheapest, and it turns "the text was wrong"
into a frame number); the state trace and diff second (they are what make
step 4 and the cross-gen policy possible); the HDD hash and the poll key
third. Each is a C change and a rebuild; none needs a regeneration. The mark
needs one player session to try, and that session should be recorded on
whatever gen is current then.

**16:18 — the first two are built, tested and in the player's bundle.**

- **Mark: press `M` in the game window.** It writes `#!mark <frame> M` into
  the open recording (flushed at once) and `[PAD-MARK] #k frame=… t=…` to
  the log; a replay echoes each mark at its frame; the harness verdict lists
  them. `jsrf_pad_record` proves a marked recording replays identically and
  that the mark comes back from the file. `~/jsrf-build/JSRF.app` was
  rebuilt from this binary at 16:18 and is current.
- **State trace: `RECOMP_STATE_TRACE=<path>`.** One line per guest frame:
  scene anchor, playtime, running indirect-call count, running draw count.
  `state_trace_diff.py a b [--marks rec]` names the first frame each field
  differs, tells drift from a step, and finds the nearest mark.
- **The floor, measured 16:27, and it is the whole signal.** Two 240 s runs
  of `gameplay_nobarrage.pad` on the same build: indirect calls differ from
  **frame 1** (3,019 vs 3,013), draws from frame 2, the per-frame call gap
  swings by ±2–3k every few frames (work straddling the flip), and the runs
  reach the first scene **50 frames apart** (f2471 vs f2521) with different
  frame counts in 240 s (14,400 vs 14,067). So a per-frame cumulative trace
  of two free-running runs cannot name a first divergence: boot pacing
  misaligns them before gameplay starts, and the scene anchor reads 0 in a
  scripted run because it is save-data. **What step 4 needs instead:** key
  the trace on the recording's checkpoint runs (input-defined, so aligned by
  construction across replays of the SAME recording), record per-run deltas
  not cumulative counts, and compare only replays. The frame-keyed trace
  stays as a raw instrument; `state_trace_diff.py` should grow a
  `--align-on-runs` mode that reads the `.padrec` and folds frames into its
  runs before diffing. Not started.

## G24 — "the wire fence transparency bug is back", 16:28

The player saw it **in a harness window**, not in the bundle (confirmed;
no bundle session has logged since the 16:18 rebuild). What is known:

- The 16 Sep cause is not it: `[VSH] (vsh_dp_zero on)` in every harness
  run today, and the switch defaults on in code (`nv2a_vsh.c:211`).
- **The harness and the bundle render on different paths.** The player's
  `paths.conf` exports `RECOMP_METAL_FF=1` (fixed-function vertex work on the
  GPU); the harness does not, so its fixed-function batches take the CPU
  path. `d911171` is the commit where "the GPU fixed-function path stops
  drawing a different picture from the CPU one" — the two paths have
  disagreed on this exact class of geometry before. The renderer's other
  self-named banners are identical between the player's 13:29 log and a
  harness run (diffed), so this is the one rendering difference.
- A CPU-vs-GPU pair (`ffpair-cpu`, `ffpair-gpu`, 200 s each, 13 captures
  each after 60 s) did not land on a fence at a comparable moment: the two
  runs drift apart in guest time before gameplay, as the floor measurement
  above predicts. Contact sheets in the session scratchpad; nothing
  conclusive in them.
- A watch run (`fence-watch`, 300 s) was put on screen with the mark key
  live; **no mark was pressed** before it ended.

**The mark now takes a picture.** `M` in any window — bundle or harness —
saves the presented frame to
`~/Library/Application Support/JSRF/marks/mark-<date>-f<frame>-<label>.bmp`
and logs the frame. It is the frame the window is fed, not the live
surface, and it can tear (main thread reads, push-buffer thread writes);
either is evidence. Not gated on `RECOMP_FB_DUMP` and never written there.

**Next, and it needs the player's eyes for one press:** run
`play_scripted.sh` again (any schedule that reaches the garage), watch for
the fence, press `M`. Then the same with `RECOMP_METAL_FF=1` exported. If
only the CPU arm shows it, the defect is in a path the player does not use
and the priority is the harness, not the picture. If both show it, it is
in the bundle too and G24 outranks G21.

**Decided by the player at 16:58: the harness is for repeating and
debugging what they see, so it renders on their path.** `play_scripted.sh`
now adopts every `export RECOMP_X=0|1` line from `paths.conf` by default —
13 switches on 19 Sep, `METAL_FF=1` among them — and never anything named
like an instrument or an output (`FB_`, `DUMP`, `WATCH`, `TRACE`,
`RECORD`, `LIFECYCLE`, `RATES`, `FRESH`, `RING`, `PATH`, `NAMES`), so a
harness run cannot write into the player's dump or recording directories.
The environment wins, so an A/B arm set explicitly stays as set, and
`JSRF_PLAYER_SWITCHES=0` runs bare. Verified on `adopt-check`: the
adopted list printed, `[VSH] fixed-function on the GPU (metal_ff on)`,
and no `FB_DUMP` or `PAD_RECORD` in the run's switch set.

**Consequence, stated once:** every frame-time number in the corpus and
every A/B before this point was taken on the CPU fixed-function path.
Numbers from after it are on the GPU path and do not compare to them.
`ab_switch.sh` remains internally valid because both arms inherit the same
adoption. The `earlyz` A/B above was the last one on the old path.

## G2 — photographed by the player's own mark, 17:40

The player pressed `M` twice in the bundle, in a session recorded to
`padrec/graffiti-2026-09-19_1739.padrec` (gen `c4400dbb0025f56d`):

| mark | frame | t | picture | shows |
|---|---:|---:|---|---|
| #1 | 3623 | 41.8 s | `marks/mark-2026-09-19_174010-f3623-M.bmp` | trick counter, no dialogue; the "3 TRICKS" HUD glyphs are the game's own |
| #2 | 5111 | 69.4 s | `marks/mark-2026-09-19_174038-f5111-M.bmp` | **Gum's bubble: "Let's see hoW much air $ou can grab."** — `w` drawn as a wider glyph, `y` as a `$`-like one, every other letter right |

**The corrupted letters are always w, x or y.** Across both sessions the
instances are `No[w]`, `ne[x]t`, `spra[y]` (13:09) and `ho[w]`, `[y]ou`
(17:40): five of five from the last four lowercase letters, and never
another letter in the same line. That is not what a random atlas-index error
looks like; it is what the *tail of a glyph page* looks like. A hypothesis,
stated as one: the text renderer fills a glyph cache page per string, w–z
land at the page's end, and when that page is recycled between boxes the
tail slots are read before they are rewritten, showing whatever glyphs sat
there — small kanji, which is exactly what the 19 Sep morning read them as.
"The wrong span moves" and "the text is animated" (`c6ed54a`) both fit a
cache that refills as the box types itself out. **Unverified.** The
measurement that would decide it: at frame 5111 of a replay of this
recording, dump the texture the bubble's glyph quads sample and the glyph
index each quad carries; the vertex-program draw path is where the text is
(8M guest VP draws in that session), not the fixed-function watcher.

**17:52 — the player's screen recording changes the picture.**
`~/Desktop/Screen Recording 2026-09-19 at 17.52.54.mov`, 2.7 s at 120 Hz on
Gum's box. Extracted frame by frame: native frames 50–57 read `hoW … $ou`,
**frames 58–60 read `how … you` correctly**, 61 onward corrupt again — one
game frame right in roughly forty. And in that same frame the name label
under the box reads **`Gum`** where every other frame reads **`m`**. The
substitution and the truncation switch off together, for one frame, as a
unit. So the state that decides both is **per frame and shared by every
glyph drawn that frame**, not per string, not per glyph index — which
retracts the glyph-page-tail hypothesis above as the *primary* mechanism
and points at the texture the glyph quads sample: on the correct frame
the atlas the GPU reads is current; on the others it is stale, and stale
in the rows that hold w, x, y and the label's first letters. The runtime's
texture cache (16 Sep: "a hit requires a full memcmp") is the first thing
to read: what it compares, over how many rows, and what refreshes it on
the one frame in forty.

**The recording is the way in.** Replay `graffiti-2026-09-19_1739.padrec`
unattended, it echoes mark #2 at frame 5111, and G23's frame window (not
built yet) would arm the texture dump for ±120 frames around it. That is the
next G2 step and it needs no player.

**The stutter, from the same live log (provisional, session still open at
17:42):** `[APU-PACE] starved=0 empty=0`, stale 0.0%, and a frame window
reading `p50=6.5 ms p90=38.0 ms over-33ms=86 of 335` beside windows at
21 ms and 18 ms — bimodal frames, exactly the tail the day handover named as
the stutter's mechanism (the CRI server's pass rate tracks the frame rate).
Still G21, not an audio bug.

## The order

1. **Replay the 13:09 recording once, before anything regenerates.** It is
   the only recording, it is bound to the current gen, and the four questions
   in the midday file's §3 are answered by it or not at all. This costs no
   player time.

   **In flight at 15:11**, as `render-investigation/replay-1309-silent`, 1300 s,
   `SDL_AUDIODRIVER=no_such_driver` so the player does not hear it. The
   runtime accepted the recording: 48,511 events over 59,112 frames, 189
   checkpoints. At t=4 min: `[JSRF-SEQ] now=30`, 7 checkpoints ok, 0 bad,
   `state=aligned in sync`, zero unresolved branches. **Caveat carried into
   the verdict:** the harness compares the run's switch set with the player's
   `paths.conf` and five exports are unset here. Three of those became code
   defaults on 19 Sep (`ee31f41`), so they are on regardless; two are
   genuinely off in this replay — `RECOMP_VSH_DP_ZERO` and
   `RECOMP_APU_LIST_MOVE_TO_FRONT`. Neither touches guest input, and the
   checkpoints are the arbiter of alignment, but the run is not
   switch-identical to the session it replays. Audio counters are void by
   construction; the crash, the recorder and the frame time are what it can
   answer.

   **RESULT, 15:33 — the replay ran the whole 1300 s and the harness's timer
   ended it, not a fault.** Verdict lines, from the completed log:

   | question | reading |
   |---|---|
   | alignment | `checkpoints ok=189 BAD=0 unreached=0 state=aligned`; recording exhausted at frame 59,112, run continued neutral to 66,469 |
   | the skating crash (G20) | **no fault**. `[ITAIL]` unresolved 0, `[ICALL]` failed 0, not-code 0, `[UNIMPL]` 0 — and the positive control is the run's own feedback dump: **444 resolved, 0 unresolved** targets written at 15:33 |
   | scene | `now=30 WaitEndStoryOrVsMission` held 1014.9 s; `off=255` retired voices (a human playthrough reads ~103): PLAYED |
   | the three promoted defaults | `[APU-IDLE-OWNER] … handoff_guard ON`; `[APU-IDLE-EDGE] reraise=95306` of 95,501 raises; `[VBLANK-REG] base=FD000000 refused=0` (the line does not literally say `upmirror=on`; that is what it prints when the mirror holds) |
   | frame time, whole run | `flips=66530 mean=19.40 ms (51.6 fps) p50=19.5 p90=26.5 p99=32.5 over-33ms=454` — the boot and title are in that mean |
   | frame time, last window | `flips=591 mean=16.92 ms (59.1 fps) p50=17.0 p99=24.0 over-33ms=0` |
   | the recorder | not exercised: `RECOMP_PAD_RECORD` was off in this run, by design (it replays) |
   | input | `no poll stall (largest gap 0.0s)`; audio OFF BY REQUEST, its counters void |

   So the 13:09 recording is now a **regression case that needs no player**:
   `play_scripted.sh <name> @…/graffiti-2026-09-19_1309.padrec 1300` with
   `SDL_AUDIODRIVER=no_such_driver`, 22 minutes, and the pass condition is
   the row above — 189/189, zero unresolved, no fault. It is bound to gen
   `c4400dbb0025f56d`; it dies at the next regeneration unless the player
   records again on the new gen, which the midday file's §3 already asks for.

   `edge_runtime_summary.py` on this log reports no reached edge, with its
   positive-control caveat printed; the feedback dump is that control.
2. **G22b's regeneration rides with the next deliberate one.** Not before.
   When it happens: run the gate (expect `todo_silent` 0, nothing else
   rising), then one scripted run under `RECOMP_UNIMPL_TRAP=1`.
3. **G21, the texture path** — the largest win and the "everything on the
   GPU" goal's real content. First step is unchanged: a run with
   `RECOMP_METAL_EARLY_Z=1`, because the early/late counter reads nothing
   with the switch off.
4. **G23's mark and state trace** — the player asked for them, they need no
   regeneration, and the mark is what makes the next G2 session cheap.
5. **G2, the text** — now known to be a wrong *span* of a whole string, moving
   over time. Aim the next capture at the speech box directly; the timing is
   known.
6. G22c, G22d, G22f as background work that needs no player.

**G21 addendum, 16:07:** the early-depth A/B is a null result by construction.
Two trials per arm at the same mission, `19.00 ms` off against `19.64 ms` on,
and **0 draws early of 1.3 million in both on-arms**: every draw carries the
guest's z-range cull (`0x1d78` nibble = CULL, the default), the shader
implements it with a discard, and a shader that may discard cannot test
depth first. The probe measured why, not how much. The refinement to try
next is treating a cull whose range is the full 0..16777215 as no cull — for
those draws the early variant is exact — with a per-draw eligibility
breakdown (alpha-tested / narrow cull / full-range cull / eligible) printed
with the switch off, so the instrument is never blind again. Runs in
`measure/earlyz_ab_20260919-155106`.

## Rules added today

- **A gate in prose is not a gate.** Two numbers that decided whether a tree
  was good sat in a goals file for a day with nothing enforcing them.
- **A comment is a no-op.** An instruction the translator cannot handle must
  leave something the runtime can report, or its failure will be found from
  the wrong end.
- **Numbers from different gens do not compare.** The manifest says which gen
  a number came from; quote it with the number, or the next reader will
  subtract two measurements of two different trees and call it progress.
- **A recording is bound to a translator hash.** Editing `tools/recomp` is
  free; regenerating is what spends the recording.
