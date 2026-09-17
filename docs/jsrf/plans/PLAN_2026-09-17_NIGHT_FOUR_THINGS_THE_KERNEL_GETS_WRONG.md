# Four things the kernel gets wrong — 17 September 2026, night

A housekeeping sweep, an upstream check, and a bug hunt in the one area the
day's work never touched: the kernel's answers at start-up. Nothing here was
found by a theory. Each item is a number that does not add up, checked against
a second source before it was written down.

Tree state at the time of writing: `main` clean at `5e47836`, level with
`origin/main`, 48/48 `ctest` green in `build-feav`, nothing running.

---

## F1 — The thunk table's own report is false, and it hides four real misses

Every run prints this, and it has been printing it for as long as the log has
existed:

    Thunk table: 143/378 resolved, 235 unresolved
    WARNING: 235 kernel imports are unresolved - game may crash!

Both numbers are wrong, and the arithmetic says exactly how. JSRF's XBE
imports **120** kernel ordinals, of which **four** do not resolve:

    91   IoDismountVolumeByName
    144  KeSetDisableBoostThread
    204  NtProtectVirtualMemory
    232  NtUserIoApcDispatcher

The loop in `kernel_thunks.c:456` walks all `XBOX_KERNEL_THUNK_TABLE_SIZE`
(378) slots, but the mapped-XBE branch is gated on `i < thunk_count`. Past
slot 120 that test fails and control falls into the **fallback reference
list** — another title's import order, kept for the no-XBE case. Slots
120–146 therefore get 27 ordinals JSRF never imported, all of which resolve;
slots 147–377 read `g_thunk_ordinals[i]` where the initialiser ran out, get 0,
and log `Unresolved kernel ordinal 0`.

It closes exactly, which is why this is a finding and not a suspicion:

    116 real imports resolved  +  27 fallback slots resolved  = 143 resolved
      4 real imports unresolved +  231 zero slots             = 235 unresolved
                                                     147 + 231 = 378

`231` is `378 − 147`, and `147` is the fallback list's initialiser count.

**What it costs today: nothing at runtime, everything in diagnostics.**
`xbox_kernel_thunk_table` is read by nothing outside `kernel_thunks.c`
(checked across `src/`, `templates/` and `tools/`), and
`xbox_unresolved_thunk` has been called **0** times in the log. So the guest
is not going through this table at all — the bridge is the live path, and it
implements 144, 204 and 232 already. Only 91 is missing from both.

But the line says "game may crash!" on every single run, and the four names
that are genuinely absent are buried under 231 lines of noise. This tree has
retired ten instruments for lying; this is the eleventh, and it is the one
printed first.

**Fix:** `break` when the mapped table is exhausted instead of falling through
to the fallback, and count only slots that came from a real ordinal. Then the
line reads `116/120 resolved, 4 unresolved` and naming those four is useful.
Ten minutes, plus a test that a short thunk table does not borrow the fallback.

**Refutable by:** a run whose report still names a count above the XBE's
import total.

## F2 — We tell the title the console is set to MONO, with AC3 on

`kernel_xbox.c`, `XC_AUDIO` (index 0x09):

    /* Stereo + Dolby Digital enabled (0x00000001 = stereo, 0x00010000 = AC3) */
    *(PULONG)Value = 0x00010001;

The comment is wrong about its own low bit. In `XC_AUDIO_FLAGS` the channel
field is `0 = stereo, 1 = mono, 2 = surround`, so `0x00010001` is **mono, with
AC3 advertised** — an encoded output path we do not have. Two independent
sources say so: the encoding itself, and `upstream/main` `8a78867`
("Advertise stereo PCM in the dashboard audio setting"), which changed this
exact constant to `0x00000000` with the note *"1 means mono; advertising AC3
requires an encoded-output path."* We do not have that commit.

This tree has already been burned once by this constant — `kernel.h:1032`
records Halo booting to the dashboard because a parental-control query was
answered with `XC_AUDIO`'s `0x00010001`.

**Why it is worth a look now rather than filing under tidiness.** G1's own
"done when" is *"the defect is a value our model presents"*, and this is a
value our model presents to the title's DirectSound before a single voice is
submitted. Speaker configuration is read at `DirectSoundCreate` time and
decides the submix topology. G1d records 3D dying first and 2D decaying after
— and the channel field is what 3D positioning is configured against.

**This is a hypothesis and it is written as one.** It is not established that
JSRF reads `XC_AUDIO` at all. The handled-index log line is `XBOX_LOG_DEBUG`
and the player's runs are at INFO, so the log cannot currently answer it.

**Measure first, in this order:**
1. Promote the query log line, or add a counter, so one existing run says
   whether index 0x09 is ever read and how early.
2. Only if it is: flip to `0x00000000`, and ask the player to listen. A
   one-line change with a `tests/` case, in upstream's own shape.

**Refutable by:** a session that never queries 0x09. That kills it in one run
and costs nothing.

## F3 — `MmGetPhysicalAddress` returns a virtual address, and JSRF imports it

`kernel_bridge.c:2208` returns the guest VA unchanged, on the reasoning that
the lower 64 MB is identity-mapped. Upstream `127f3fa` disagrees for the
contiguous arena:

    g_eax = (addr >= XBOX_CONTIG_BASE &&
             addr < XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
          ? addr - XBOX_CONTIG_BASE : addr;

We have `XBOX_CONTIG_BASE` and use it in five other places in this same file;
only this bridge ignores it. **JSRF imports ordinal 173** (confirmed against
the XBE's import table, not assumed), and the consumers of a physical address
in this title are the two device models we are currently debugging.

**Not claimed as a live defect.** Whether the guest ever passes a
contiguous-arena address here is a runtime question and nobody has asked it.
`xbox_memory_layout.c:3286` already does the inverse fold
(`XBOX_CONTIG_BASE | (put & 0x0FFFFFFF)`) for the pushbuffer, so a
compensating mask elsewhere is entirely possible and would make this harmless.

**Measure:** count calls, and bucket the argument by whether it lands in the
contiguous window. Count before enforcing — the same rule G7 is parked under.

## F4 — `fldcw` is recorded and then ignored

The lifter models `fnstcw`/`fldcw` into `g_fp_control_word`
(`lifter.py:3588–3606`). No FIST helper reads it: `RECOMP_F2I16_ROUND`,
`RECOMP_F2I_ROUND` and `RECOMP_F2I64_ROUND` all call `nearbyint` under the
**host's** rounding mode. Upstream `55aa0ba` reads the guest's RC bits
instead.

**For JSRF today this is almost certainly harmless, and the tree already
explains why.** `lifter.py:3362` records that `sub_0017C3E8` is MSVC's
`_ftol2` — 432 call sites across 129 functions, *the* float→int conversion for
the whole image — and that it does **not** reprogram the control word: it
converts nearest-even and corrects toward zero in guest arithmetic we lift
normally. That reasoning is sound and should not be undone.

The gap is narrower than the upstream fix implies, and it is real. The whole
generated image contains exactly **two** `fldcw` sites, both in
`sub_0017F03A`, which is the `_control87` pattern —
`new = (new & mask) | (old & ~mask)`, load it, return the old word. So the
title *has* a rounding-mode setter. Whether anything calls it with RC ≠ 00 is
a runtime question, and if something does, every `fist` in that window is
silently wrong by up to one unit.

**Measure, do not fix:** one counter on `g_fp_control_word` writes where
`(cw >> 10) & 3` is nonzero. If it never fires, write that down in
`recomp_types.h` beside the nearest-even argument and the question is closed
for good. If it fires, `recomp_fist` is the shape of the answer — but at our
widths and with our indefinites, not upstream's.

---

## Upstream and the other checkouts

**`sp00nznet/xboxrecomp`**: 80 commits ahead of our merge base, we are 500
ahead of them. HEAD is v0.10.0 "Negative Control". **PR #57 (our three x86
semantics fixes) is MERGED.** We have no other PR and no issue open there.

The merge recommendation in
`PLAN_2026-09-17_UPSTREAM_STATUS_AND_WHAT_WE_OWE.md` still holds — 14
conflicts including `lifter.py` and `translator.py`, and merging those
invalidates every archived gen. **F2 and F3 change that plan in one respect:**
two of the 80 are small, self-contained kernel fixes for files we can take
individually. Cherry-pick the two lines; do not merge the 80.

**What we still owe them, unchanged and unsent.** The mixbin discard is
ranked first in that plan — a handful of lines, confirmed present at
`upstream/main:src/apu/apu_dsp.c:150`, silently dropping every 3D voice in any
title, with a player who confirmed the fix by ear. It has not been sent. It is
the smallest, clearest, most generally useful thing in this repository and it
has been sitting here for a day.

**`~/jsrf-build/upstream-contrib`** is parked on `contrib/x86-semantics-fixes`
at `366c8c5`, level with `fork/x86-semantics-fixes`, and that work is merged
upstream. The checkout is finished; it is the natural place to raise the
mixbin PR from.

**`~/evo-jsrf-bench`** — three commits, **no remote configured**, and a dirty
tree (`.gitignore`, `AGENTS.md`, `README.md`, `harness/jsrf-debug.md` modified,
`harness/qwen-task-discipline.md` deleted, `benchmark/` and
`harness/task-discipline.md` untracked). Nothing there can be pushed anywhere
until it has a remote. Not this repo's problem, but it is unbacked work.

## Housekeeping

- `main` was already clean and level with `origin/main`; nothing was pending.
  The memory index's pointer to the active goals file was two files stale and
  has been corrected to `JSRF_GOALS_2026-09-17_THE_GUEST_STOPS_TALKING.md`.
- `~/jsrf-build/jsrf-first-fault` holds **9.1 GB** across 20-odd build
  directories, `frame-bisect` (2.7 GB) and `render-investigation` (2.0 GB)
  being the two largest, plus two 5.0 GB `play-hdd` trees and three copies of
  `JSRF.app`. 782 GB free, so this is not urgent and nothing was deleted —
  archived gens and preserved baselines are read-only by policy and a build
  directory is not always distinguishable from one at a glance. Flagged for a
  decision, not acted on.
- `~/jsrf/JSRF-US.xiso.iso` (7.4 GB) duplicates the iCloud copy byte-for-byte
  by size. Same call to make.
- `~/jsrf/icall_targets.dump` and `~/jsrf/xbox_kernel.log` are 15 Sep leftovers
  from before the repo moved; the live ones are inside the repo and gitignored.

---

## The plan

**Track A stands.** `PLAN_2026-09-17_WHAT_ACTUALLY_MOVES_THE_NEEDLE.md` ranks
`nv2a_metal_sync_range` first, with a completed measurement saying GO
(`[NOSYNC]` p50 8.0 ms against an 18.5 ms median) and a correctness bug
attached to it. Nothing found tonight outranks it, and nothing found tonight
needs a player session. **Do A1 first and do not let this document interrupt
it.**

Everything below is an hour of work in total and none of it is a theory:

1. **F1, the thunk report** (~10 min). Pure diagnostic integrity, no runtime
   behaviour change. Do it first because it is the line every future run
   prints first, and because a false "may crash" costs a future session real
   time.
2. **F2 step 1, the `XC_AUDIO` probe** (~5 min). Promote one log line. This
   buys the answer to a live G1 question — *does the title ask us how many
   speakers it has, and what do we say* — for the price of a log level.
3. **The mixbin PR to upstream** (~30 min). Owed, ranked first in its own
   plan, unsent, and the branch to raise it from already exists.
4. **F4's counter, then F3's counter** (~10 min each). Both are "count before
   enforcing". Both close a question permanently if they read zero.

**F2 step 2 — flipping `XC_AUDIO` to stereo PCM — is gated on step 1 reading
nonzero, and then on a player listen.** A player-facing default needs a
picture or a listen, not a residual. That rule cost two backed-out switches on
16 Sep and it is not being bent for a promising one-liner.

## What this plan deliberately does not do

- **It does not touch `src/` tonight ahead of Track A.** Four findings is a
  good evening; spending Track A's slot on them would repeat exactly the
  mistake `WHAT_ACTUALLY_MOVES_THE_NEEDLE` diagnosed — de-risked work crowded
  out by newer, more interesting work.
- **It does not merge upstream.** Two cherry-picks, if the counters justify
  them. Not 80 commits and not `lifter.py`.
- **It does not claim F2 explains the music death.** Five theories died on
  17 Sep. This one comes with the measurement that refutes it attached, and
  that measurement runs first.
