# What xboxrecomp would take — 21 September 2026

Scoping only. Nothing here was implemented; a `still-census` run was in flight
(pid 83565) for the whole of this review, so no `*.c`/`*.h`/`*.m` under `src/`
or `diagnostics/jsrf_first_fault/` was touched.

Question asked: *"would any fixes to xboxrecomp be possible/help?"*

Short answer: **yes, three of them, and one is a real correctness bug that
silently discards pixels for any title that uses an Xbox lock flag.** Two of
the five leads handed to this review are **not real as stated** — the defect
they describe was already fixed or never existed — but one of those two hides
a sharper defect underneath it, and that one is the most valuable item on the
list.

## Upstream is receptive, and the PR shape is known

`upstream/main` carries `Merge PR #86`, `Merge PR #87` and
`Credit the 25 merged PRs in CONTRIBUTORS.md`. This is a maintained project
that takes outside patches.

The template is set by this repo's own prior contributions, and it is tight:

| branch | files | diff |
|---|---|---|
| `fix-debugbreak-non-msvc` | `src/platform/xbox_winnt.h` | 1 file, +10 −2 |
| `memory-safety-guest-buffers` | `src/kernel/kernel_bridge.c` | 1 file, +101 |

**One file, one commit, one defect, a title-independent justification.**
Everything recommended below is cut to that shape. Anything that cannot be
is marked DEFER or DROP.

## Ranking

| # | Lead | Real? | Upstream/local | Effort | Recommendation |
|---|---|---|---|---|---|
| **1** | NV2A unhandled-method census ranks by frequency and truncates | **Yes — new** | Upstream | ~1 h | **RECOMMEND — do this first** |
| **2** | Wrong D3D8 lock bits in `d3d8_xbox.h` | **Yes** | Upstream | ~1 h | **RECOMMEND** |
| **3** | CFG recovery and the lifter disagree on jump-table forms | **Yes — new** | Upstream | ~4 h | **RECOMMEND (bundle with the owed regen)** |
| **4** | `return "reason"` sites carrying no value | Partly | Mostly local | ~6 h | **DEFER** |
| 5 | `0x1D70` fence decode as an upstream patch | Yes, but | Upstream | ~30 m | DEFER (rides #1) |
| 6 | `JSRF_MEM_WATCH` dead CMake option | **Misattributed** | **Local, doc-only** | ~10 m | **DROP as upstream; fix the comment locally** |
| 7 | "Unknown NV2A methods are silently dropped" | **Not real** | — | — | **DROP** |

---

## 1. The census ranks by frequency, and that is why `0x1D70` survived

**NEW — not on the brief's list. Strongest item here.**

This is the finding that falls out of checking lead 3, and it inverts it.

### The brief's premise was wrong, and the truth is worse

The brief asked: *"is the executor silently dropping unknown methods? If
silently dropped, that is itself the upstream defect."*

**It does not silently drop them.** `src/kernel/nv2a_pb_exec.c:1446`
`note_unhandled()` maintains a full census — every undecoded method, counted,
up to `PB_EXEC_MAX_UNHANDLED` = 2048 distinct entries
(`nv2a_pb_exec.c:624`). The shutdown report prints the totals
(`nv2a_pb_exec.c:5796`):

```
[GPU] surface ... clears %u | %u unhandled methods (%d distinct)
```

So "count the input" was **already implemented here**, years before it was
named as a method. Method `0x1D70` fell through `default:`
(`nv2a_pb_exec.c:5344`), matched none of the four range checks, and reached
`note_unhandled()` on every single frame the title ever rendered.

**It was counted the whole time. The report just never showed it.**

### The actual defect: the report is truncated by frequency

`nv2a_pb_exec.c:5875-5899` prints only the top N by count, N defaulting to
**10**:

```c
const char *e = getenv("RECOMP_PB_EXEC_TOP");
top = e ? atoi(e) : 10;
```

And the design comment above the census (`nv2a_pb_exec.c:620-622`) states the
principle that makes that truncation seem reasonable:

```c
/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
```

**That comment is the bug.** `0x1D70` is
`BACK_END_WRITE_SEMAPHORE_RELEASE` — the one method in the stream that tells
the guest its work is finished. It fires roughly **once per frame**. It
competes in that ranking against `SET_VERTEX_DATA*` and friends, which fire
**millions of times**. Frequency ranking buries it by construction, and it
stayed buried for the entire life of this runtime while every
`D3D_BlockOnTime`, every `D3DVertexBuffer_Lock` and every
`D3D_BlockOnResource` in the title returned instantly (see commit `5358eec`).

The same truncation is currently hiding `NV097_BREAK_VERTEX_BUFFER_CACHE`
(`0x1710`) — verified absent from the whole of `src/`, so it too reaches
`note_unhandled()` and is counted, and it too is emitted by every text buffer
lock.

**A census that is ranked and cut by frequency is a rejection path that names
the gate and not the value, one level up.** The project's own rule caught the
symptom; this is the rule applied to the reporting layer, which is where it
actually failed.

### Why it is upstream

`nv2a_pb_exec.c` is **in `upstream/main`** (verified via `git cat-file -e`).
Semaphore release is generic NV2A hardware, not a JSRF quirk: any title using
`D3D_BlockOnTime`, `D3D_BlockOnResource` or a fenced dynamic-buffer lock hits
this. Any downstream port of this runtime has had a vacuous GPU fence and no
report that would say so.

### Shape and effort

**~1 hour, one file.**

- Print **all** `s_unhandled_count` distinct methods by default. The data is
  already retained (2048 slots), the distinct count is already printed, and
  the report already runs only once every few seconds — so this is free.
  Keep `RECOMP_PB_EXEC_TOP` as a *cap* for people who want the old behaviour.
- Rewrite the `620-622` comment to say why frequency is the wrong sort key,
  citing `0x1D70` as the worked counterexample.
- Optionally flag a small set of known-semantically-critical methods
  (semaphore release, cache break, flush) so they print regardless of rank.

**Risk: very low.** Diagnostic output only. No behaviour change, no
regeneration, no effect on any pad recording.

This is also the highest-leverage patch on the list, because it is the one
that *finds the next four bugs* rather than fixing one.

---

## 2. The lock bits in `d3d8_xbox.h` are the PC ones, and one of them is live

**Real. Verified. Upstream. This is a correctness bug, not a latent one.**

### Evidence

`src/d3d/d3d8_xbox.h:715-718`, under a header whose name claims to be the
Xbox variant and under the comment `/* Lock flags */`:

```c
#define D3DLOCK_READONLY    0x00000010
#define D3DLOCK_DISCARD     0x00002000
#define D3DLOCK_NOOVERWRITE 0x00001000
#define D3DLOCK_NOSYSLOCK   0x00000800
```

Those are PC Direct3D 8 values. The XDK 4134 bits, read out of the title's own
statically linked D3D8 and recorded in
`HANDOVER_2026-09-21_COUNT_THE_INPUT_NOT_THE_GATE.txt` section 4, are:

```
0x10 NOFLUSH   0x20 NOOVERWRITE   0x40 TILED   0x80 READONLY
```

### Blast radius — narrower than it looks, and worse where it lands

Only **one** of these four constants is ever read. `D3DLOCK_READONLY` is
consumed at three sites, all in `src/d3d/d3d8_resources.c`:

- `:1179` — P8 path, `sf->lock_readonly = (Flags & D3DLOCK_READONLY) != 0;`
- `:1240` — map mode, `(Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE`
- `:1249` — general path, same assignment as `:1179`

`lock_readonly` then gates the **write-back** at `sf_UnlockRect`
(`:1269` for P8, `:1288` for the general path). `DISCARD`, `NOOVERWRITE` and
`NOSYSLOCK` are defined but never read anywhere in the tree — those three are
purely latent.

So the live consequences for **any** title are:

- **A lock with real Xbox `NOFLUSH` (0x80's neighbour, 0x10) is misread as
  READONLY.** The surface is mapped `D3D11_MAP_READ` and, critically, the
  write-back at `:1288` is **skipped**. Everything the title writes into that
  locked surface is silently discarded. `NOFLUSH` is an Xbox-specific
  extension meaning "do not stall the command buffer" — it is a perfectly
  ordinary flag to pass on a dynamic texture you are about to fill, and a
  title doing so gets nothing drawn, with no error and no log line.
- **A lock with real Xbox `READONLY` (0x80) is not recognised.** The surface
  is mapped `READ_WRITE` and written back on unlock. Data stays correct; the
  cost is a needless full-region `CopySubresourceRegion` on every read-only
  lock.

One direction loses pixels; the other wastes bandwidth. Both are wrong for
every title.

### Why JSRF is unaffected

The handover closed this: JSRF's text path locks with `Flags=0` — a genuinely
blocking, whole-buffer lock, `OffsetToLock` hardcoded 0. `Flags=0` misses every
one of these constants, so JSRF never exercises the defect. That is exactly
what makes it an upstream item and not a local one: **it costs this project
nothing and costs the next title its graphics.**

### Shape and effort

**~1 hour, one file** (`d3d8_xbox.h`), plus optionally a short comment at the
`d3d8_resources.c` consumer noting that these are Xbox bits.

Correct the four values, keep the PC values in a comment for anyone who cross-
references MSDN, and cite the XDK version the bits were read from. The prior
`fix-debugbreak-non-msvc` PR is the precedent: a one-file header correction
with a platform-truth justification.

**Risk: low, but not zero.** Changing `D3DLOCK_READONLY` from 0x10 to 0x80
changes behaviour for any title that was accidentally relying on the wrong
bit. Worth one paragraph in the PR body and a run of the D3D tests. A JSRF
regression run is cheap insurance but is not strictly required, since JSRF
passes `Flags=0`.

---

## 3. CFG recovery and the lifter disagree about what a jump table looks like

**NEW — not on the brief's list. Real, upstream, and it serves G22 directly.**

The brief asked whether there is a mechanism to feed externally-known
jump-table addresses as hints, and said to report it only if the plug-in point
could be named concretely. It can — and the more interesting finding is that
the mechanism already exists on the call side and the gap is elsewhere.

### What already exists

`tools/recomp/icall_feedback.py` (**in `upstream/main`**, 379 lines) is a
complete runtime-feedback loop for indirect **call** targets, explicitly
modelled on Microsoft's `VirtualDispatchTraceFiles` / `UpdateEnlightenments`.
Build with `RECOMP_ICALL_FEEDBACK`, dump, `merge`, then feed
`tools/disasm --seed-functions`. The database is cumulative and the loop
converges. **The hint mechanism for indirect calls is not missing.**

The lifter also already resolves intra-function switch tables:
`lifter.py:2916` `_analyze_switch_table` and `lifter.py:2896`
`_read_jump_table`, emitting a compare chain plus a `RECOMP_ITAIL` fallback
(`lifter.py:2991-2999`).

### The real gap: two detectors, two different definitions

The lifter reads its tables from `self.jump_table_targets`
(`lifter.py:1556`, consumed at `:2928`), which is populated **only** from
`translator.py:945`:

```python
self.lifter.jump_table_targets = (
    recovered["jump_tables"] if recovered else {})
```

`recovered["jump_tables"]` comes from `translator.py:_recover_cfg`
(`:678-720`). And that discovery loop is **strictly narrower** than the
lifter's own detector it feeds:

| | `translator.py:_recover_cfg` (`:700-702`) | `lifter.py:_analyze_switch_table` (`:2925-2927`) |
|---|---|---|
| addressing form | `if not operand.mem_index or operand.mem_base: continue` — **requires an index, rejects any base register** | `if not op.mem_disp or not (op.mem_index or op.mem_base)` — **accepts base-only** |
| table location | `if not (start <= table_va < upper): continue` — table must lie **inside the function** | no such constraint |

So `jmp [base + disp]` and `jmp [base + idx*4 + disp]` dispatches are invisible
to CFG recovery, and any table placed in `.rdata` rather than inline is
invisible too — even though the lifter downstream is perfectly capable of
handling both once it has the entries. Every such dispatch degrades to an
unresolved `RECOMP_ITAIL`, which is precisely the population G22's `[ITAIL]`
counter exists to measure, and whose failure path
(`recomp_types.h:1445-1453`) does `g_esp += 4; g_eax = 0` and logs — i.e.
takes a wrong branch and carries on.

This is a genuine upstream defect: two places in one pipeline that disagree
about the same construct, with the narrower one gating the wider one. It is
title-independent — MSVC emits both forms — and `_read_local_jump_table`'s own
docstring shows the author already thought hard about biased origins, so the
asymmetry looks like an oversight rather than a decision.

### The regeneration cost is currently ZERO, and that is the whole argument

The brief flagged that any change under `tools/recomp` changes
`JSRF_GEN_TRANSLATOR` and that every binary built on a new generation refuses
existing pad recordings. True — and already paid for.

`JSRF_GOALS_2026-09-19_EVENING_EVERY_EDGE_IS_COUNTED.md` (the newest goals
file, so the active one) records under "THE ORDERING CONSTRAINT, restated for
G22":

> G22b below changed `tools/recomp/lifter.py`. It is **not regenerated**, on
> purpose. [...] Two configure-time warnings now fire and are expected until
> the next regeneration [...] **Do not regenerate to silence them.**
> Regenerate when the 13:09 recording has been spent [...] **and bundle every
> pending translator change into that one regeneration**, as the midday
> file's rule says.

The tree is **already stale by design**, a regeneration is **already owed**,
and the standing instruction is to **bundle**. The marginal cost of adding
this change to the pending batch is therefore **nothing**. Deferring it does
not avoid a regeneration; it only risks missing the bundle and paying for a
second one later.

### Shape and effort

**~4 hours**, `tools/recomp/translator.py`, plus a test alongside
`test_intra_indirect_jmp.py` / `test_cfg_recovery.py`.

Relax `_recover_cfg`'s operand filter to match the lifter's, and allow a table
VA outside `[start, upper)` while keeping the existing "targets must land
inside the function" truncation rule that `_analyze_switch_table` already
enforces — that rule, not the table's own address, is what keeps garbage out.

**Do NOT add an external jump-table hint file.** It would be a second
configuration surface for something static analysis can find once the filter
is fixed, and `icall_feedback.py` already covers the case where runtime
observation is genuinely required. Fix the detector; do not add a way to
hand-feed it.

**Risk: moderate — this is the only item that changes generated code.** Gate
it behind the existing test suite, and land it in the owed bundle, not
standalone. A wider detector can only *add* resolved arms (the
"targets inside the function" rule is unchanged), so the failure mode is a
table that resolves where it previously degraded to `ITAIL` — observable as
`[ITAIL]` going down, which is exactly G22's metric.

*Note: the symbol-side of this question is being analysed separately; this
section is deliberately confined to the lifter/translator.*

---

## 4. "Every refusal carries its refused value" — right principle, mostly local code

**Partly real. The principle generalises; the code it would apply to mostly
does not belong to upstream.**

### The count the brief asked for

`grep -rn 'return "'` gives **91** sites in `src/nv2a` (64) and `src/kernel`
(27). That number is misleading — **37 of them are name-lookup helpers**, not
refusals:

| file | sites | kind |
|---|---|---|
| `src/nv2a/nv2a_texture_copy.c` | 35 | **refusal** |
| `src/kernel/nv2a_pb_exec.c` | 12 | **refusal** (DMA range / overlap checks, `:1339-1405`) |
| `src/nv2a/nv2a_ff.c` | 7 | **refusal** |
| `src/nv2a/nv2a_vsh_hlsl.c` | 11 | name lookup (`return "oPos"` …) |
| `src/nv2a/nv2a_vsh_msl.c` | 11 | name lookup |
| `src/kernel/xbox_memory_layout.c` | 8 | name lookup (USB status strings) |
| `src/kernel/kernel_thunks.c` | 6 | name lookup (log levels) |
| `src/kernel/nv2a_pb_scan.c` | 1 | `return ""` |

**54 genuine refusal sites.** Of those, roughly **8 are already
instrumented** — `s_rej_inreg`, `s_rej_const`, `s_rej_texmode`,
`s_rej_texstage`, `s_rej_alpha`, `s_fmt_rejected`, `s_dma_rejected` — leaving
**~46 that still name the gate and not the value.**

### Why it is mostly not an upstream patch

File ownership decides this, and it splits badly:

- `src/nv2a/nv2a_texture_copy.c` — **LOCAL ONLY**, not in `upstream/main`.
  35 of the 54 refusal sites, and both existing censuses (`[TEXFMT]`,
  `[COMBINER]`), live in a file upstream does not have.
- `src/nv2a/nv2a_ff.c` — **LOCAL ONLY**. 7 more.
- `src/kernel/nv2a_pb_exec.c` — **IN UPSTREAM**. 12 sites.

So **42 of the 54 sites are in files upstream has never seen.** Sending this
upstream as a code change would mean sending files, not fixes.

### What *is* upstream-shaped

Two things, and neither is the mechanical sweep:

1. **Item 1 above.** The census-truncation fix is this principle applied at
   the one place in an upstream file where it demonstrably cost the project
   months. It is the highest-value instance and it is already recommended.
2. **The principle itself, as documentation.** `CONTRIBUTING.md` is the
   natural home for "every refusal records the refused value, and a census is
   never ranked by frequency alone". That is a genuine contribution, costs
   nothing, and does not require moving any local file upstream.

### Shape and effort

**~6 hours** for the full local sweep of 46 sites; mechanical but not
trivial — each needs a counter, a bucket key chosen to be meaningful, and a
report line that stays silent when clean (the discipline both existing
censuses follow).

**DEFER.** Do it locally, incrementally, when a specific gate is next
suspected — that is how both existing censuses were born, and it is how they
stayed useful instead of becoming noise. A speculative sweep of 46 sites
produces 46 counters nobody reads. **The principle belongs upstream as
documentation; the code does not belong upstream at all.**

---

## 5. The `0x1D70` fence decode, as a standalone upstream patch

**Real, upstream-worthy, but subsumed.**

Commit `5358eec` added `case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:` calling
`d3d8_ring_fence_release(param)` at `nv2a_pb_exec.c:5083`, in an upstream
file, fixing generic NV2A hardware behaviour. The in-tree comment
(`:5057-5081`) is unusually good: it explains why writing the fence on the
pusher thread in stream order is what makes it honest, and why the
`SET_SEMAPHORE_OFFSET` path in `nv2a_pgraph_d3d11.c` cannot serve (this title
sets offset 0 and puts the base in the semaphore DMA object, which nothing
resolves — that path logs `semaphore offset 0x00000000 not usable` and signals
nothing).

It is a good patch. But it calls `d3d8_ring_fence_release`, which couples it
to the D3D8 ring layer, and upstream will want to know how that interacts with
the `nv2a_pgraph_d3d11.c` sink before taking it. That is a design conversation,
not a one-file fix, and it does not fit the established PR template.

**DEFER** — and note that **item 1 delivers most of its value upstream at a
fraction of the cost**, because a downstream port with an untruncated census
will *see* `0x1D70` sitting there uncounted and can decide for itself. Ship
the instrument; let the fix follow with a maintainer in the loop.

---

## 6. `JSRF_MEM_WATCH` — the finding is real, the attribution is wrong

**Verified as a fact. Misattributed as a defect. Not upstream. Not even much
of a local bug.**

The handover says (section 4): *"xbox_kernel exports
`RECOMP_MEM_WATCH_RUNTIME=1` as PUBLIC and xboxrecomp is an INTERFACE library,
so the define reaches the generated code transitively."*

**Every mechanical claim checks out:**

- `src/kernel/CMakeLists.txt:75` — `target_compile_definitions(xbox_kernel PUBLIC RECOMP_MEM_WATCH_RUNTIME=1)`
- `CMakeLists.txt:62` — `add_library(xboxrecomp INTERFACE)`, linking `xbox_kernel` at `:63-64`
- `diagnostics/jsrf_first_fault/CMakeLists.txt:455-459` — the `JSRF_MEM_WATCH` option, which adds the *same* define `PRIVATE`

So yes: the option is dead, the define arrives transitively regardless, and
the hook is in every build.

### But the PUBLIC export is deliberate, and documented in place

`src/kernel/CMakeLists.txt:72-74`, directly above the line:

```cmake
# Generated store helpers call into xbox_kernel only on normal game targets.
# Standalone conformance harnesses include recomp_types.h without this public
# definition and retain direct, dependency-free stores.
```

That is a designed contract, not an accident. `PUBLIC` is doing exactly the
job it was chosen for: link `xbox_kernel` and you get the traced store; build
a standalone harness without it and you get a plain store with no undefined
tracer symbols. `templates/runtime/recomp_types.h:633` documents the same
split from the header side. **Changing `PUBLIC` to `PRIVATE`/`INTERFACE` would
break the contract, not fix it.** The brief's suggested shape is the wrong
fix.

### The performance cost is real, small, and acknowledged

`recomp_types.h:641-652`: the hook is **runtime**-gated on
`g_recomp_mem_watch_enabled`, so the compiled-in cost is one
perfectly-predicted, never-taken branch per translated store. The header calls
it "one predictable disabled-path branch" (`:632`); the diagnostics file calls
it "one predictable, perfectly-predicted branch on every guest store, which is
not free". Both are honest. It is not free and it is not much.

### The only actual defect is a false comment in a local file

`diagnostics/jsrf_first_fault/CMakeLists.txt:453` promises:

```
# It rebuilds every generated translation unit, so expect a full build.
```

That is false — the define is already present, so toggling the option changes
nothing and rebuilds nothing. The cost is exactly what the handover says: one
future session burns a pointless full build believing the comment.

**`diagnostics/jsrf_first_fault/` is a JSRF-local tree.** Nothing here is
upstream's problem.

**DROP as an upstream item. Fix locally, ~10 minutes**, as a comment
correction plus either deleting the dead option or making it honest
(`message(WARNING ...)` saying the define already arrives via `xbox_kernel`).
Blocked until the current run lands — that file is under the
do-not-touch path.

---

## 7. "Unknown NV2A methods are silently dropped"

**Not real. Dropped.** See item 1: `note_unhandled` has counted every one of
them all along, into a 2048-entry table, with the distinct count printed at
shutdown. The defect is in the *report*, not the *counting*. Recorded here so
the claim is not re-investigated a third time.

---

## Recommended order

1. **Item 1** — untruncate the census (`nv2a_pb_exec.c`). One file, ~1 h,
   zero risk, no regeneration. Ship first: it is the instrument that finds the
   rest.
2. **Item 2** — correct the lock bits (`d3d8_xbox.h`). One file, ~1 h. The
   only item that is a live correctness bug for other titles, and the one that
   most clearly belongs to someone other than us.
3. **Item 3** — widen the CFG-recovery jump-table filter
   (`translator.py`). ~4 h. **Must ride the already-owed regeneration
   bundle** per the active goals file; do not regenerate for it alone.
4. **Item 4** — send the principle to `CONTRIBUTING.md`; keep the sweep local
   and opportunistic.

Items 1 and 2 are independent, need no regeneration, invalidate no pad
recording, and can be prepared in the existing
`build-macos/upstream-contrib-wt` worktree without disturbing the current run.
Item 3 is the only one that touches generated code, and its cost is currently
zero only because a regeneration is already owed — that window is the thing to
not miss.

## One caveat on method

The two "not real" findings (items 6 and 7) both came from handover claims
that were mechanically accurate and causally wrong — the `PUBLIC` export is
real and deliberate; the methods are undecoded and counted. Both were caught
by reading the code immediately around the cited line rather than the line
itself. That is the same failure mode this project already named: **a claim
that names the location and not the reason is a blind spot.** The handover was
a good starting point and should not be treated as a finding list.
