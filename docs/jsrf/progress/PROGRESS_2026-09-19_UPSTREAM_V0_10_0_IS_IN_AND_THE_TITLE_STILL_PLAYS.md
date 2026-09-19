# Upstream v0.10.0 is merged, and the title was run against it

19 Sep 2026. 80 upstream commits, no textual conflicts, 82 files, +10,388/-204.

The merge itself was done on a branch and came back green under `ctest` — but
`ctest` builds the *previous* generation. It cannot see the translator at all,
because the generated C it compiles was produced before the merge. So a green
`ctest` on a merge that touches `lifter.py` (+325), `translator.py` (+121) and
`recomp_types.h` (+75) is not evidence about the recompiler. That is the gap
this note closes.

## What was wrong before it could land

Five pytest cases were red. Two had been reported as "pre-existing" without
saying which, so all five were re-run against pre-merge `main` first. They
turned out to be four unrelated things and exactly one of them was a real
decision:

**`_pass_imm_ref_targets`, the merge's declared lowest-confidence call.** Our
tree lets only a recognisable prologue realign the sweep over an address taken
as an immediate; upstream also allows a constant-return stub and a virtual-call
thunk. Ours was kept, because 0x1600CA — the `C9` byte of `test ecx, ecx`,
which also decodes as `leave` — is the reason the guard exists at all, and
promoting it once split QueryInterface and lost its stack cleanup.

Measured instead of argued. Discovery run twice over the title's XBE with
identical seeds, once with each form:

| | functions | changed bounds | QueryInterface |
|---|---|---|---|
| ours (prologue only) | 8,866 | — | 0x00160080–0x00160108 |
| upstream (3 shapes) | 8,866 | 0 | 0x00160080–0x00160108 |

Not one address added, not one lost, not one boundary moved. `leave` is
neither a constant stub nor a vcall thunk, so the shape that caused the
original split cannot satisfy the widened test either. Upstream's form is now
ours — it costs this title nothing and it is what finds MSVC's vtable-only
accessors in other ones — and the measurement sits in the comment so the next
merge does not re-litigate it.

The other three were not defects in this tree:

- **`fnstsw [mem]`** demanded the old `g_fp_cmp` spelling. Upstream moved the
  C3/C2/C0 values into `RECOMP_FCMP_CC`, which `fxam` writes too; the merge
  updated the `fnstsw ax` test for that and missed its neighbour. The literals
  it used to string-match are now exercised by running the emitted code at
  every value of TOP in `test_x87_classification.py`.
- **`test_avsendtvencoderoption_route_and_abi`** matched
  `stdcall_args_for_ordinal` without the `static int` its sibling four lines up
  has, so the non-greedy match began at the first *mention* of the name — a
  comment 4,500 lines earlier — and searched an unrelated function body for
  `case 2:`. The table has said `case 2: return 16;` all along. Predates the
  merge on both sides.
- **`onexit`** is MSVC's `<stdlib.h>`. On this libc `void onexit(void);` is an
  ordinary identifier, so upstream's negative control cannot fire here. It
  stays in the reserved set, because that set covers every toolchain the
  generated C is built with; the control now excuses a clean compile only after
  probing that the platform does not declare the name at all, so it still bites
  where it is supposed to.

`tools/` is 481 passed, 2 skipped, 604 subtests.

## Then the part ctest cannot do

Full regeneration against the merged translator, from the game dump:

```
translator_sha     cf24eed44d1bcd3c -> b6f29140efaf1b21
runtime_types_sha  10843754e9beb3c6 -> 9a5dbe816190636e
xbe_sha            bb2410618c35ccab (unchanged)
git_dirty          no
```

Discovery converged in 3 rounds at 8,866 functions. The mandatory
mid-function post-pass ran and recovered 135 entries plus 11 they reference.
`build-feav` rebuilt from scratch, 0 errors. `ctest` 70/70 — this time against
generated C that the merged translator actually produced.

The previous gen is preserved at `build-macos/jsrf-first-fault/gen.pre-upstream-v0.10.0`.

Then a scripted 180 s gameplay run, `play_scripted.sh` with `gameplay.pad`:

```
4,281 flips, final window 60.1 fps (p50 16.5 ms, over-33ms=0)
audio device open at 48000 Hz, 2ch, 512-sample buffer
[JSRF-SEQ] reached state 30 WaitEndStoryOrVsMission -- in a mission
104 pad events fired, no input-poll stall (largest gap 9.0 s)
0 faults, 0 unresolved calls, 0 bad pushbuffer headers
```

The harness scores the *playing* as MARGINAL (`off=7` against the `off=103` a
person produces), which is the usual verdict for a pad script and is a
statement about the schedule, not about the merge. What it establishes is the
thing that needed establishing: the title boots, renders, presents, reaches
gameplay and produces audio on a binary whose every guest function was emitted
by the merged translator.

## What is not yet measured

A human playthrough. Everything above is a scripted schedule, so nothing here
speaks to the glyph defect, the stale-buffer census or the `sge_oob` verdict on
G7 — those all wanted a player's run and still do. The bundle, `paths.conf` and
the glyphdump directory are staged for it.
