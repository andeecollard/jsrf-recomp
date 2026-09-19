# Plan, 19 September 2026 — the audio path, and what we can send upstream

Written after a session that fixed nine things and, more usefully, read four
instruments that were already armed and had never been looked at. Ranked by
value per unit of cost. Everything here is measured unless it says otherwise.

---

## Where the evidence now points

Three findings from today, all from existing logs, all pointing at the same
subsystem:

1. **Nine of eleven producing voices are replaying stale buffers** — 99.3% to
   99.9% — while **voice 68 sits at 4.3%** in the same run on the same
   instrument. Voice 3 replayed one 32-sample slot for **65,933 ms**.
2. **193,693 ADPCM blocks refused (4.4%)**, every one silenced, and the first
   failure is **exactly 11 blocks before the end of every voice's buffer** —
   seven voices, buffers from 137 to 219 blocks, the same 11 each time.
3. All 464 recorded failures read the **same** header, `0x08080808`. One
   repeated value across 14 voices is filler, not audio at a wrong offset.

A constant *difference* is not a stride error and 11 is not a page of blocks,
so this is a **fixed over-read**: the fetch runs off the end of the real data
by the same distance every time.

---

## 1. G7's dead bound is the best candidate for the over-read *(fix, cheap)*

`get_data_ptr` (`apu_vp.c`) is the scatter-gather translation:

```c
unsigned int entry = addr / TARGET_PAGE_SIZE;
assert(entry <= max_sge);
```

**Both call sites pass `0xFFFFFFFF`**, so the bound can never fire, and a read
past the end of the page table returns a translation built from whatever dword
happens to sit after it. The ADPCM block fetch is one of those two call sites.

That is the shape of the finding: read past a boundary, get a plausible-looking
but wrong page, decode filler. It is already on the books as **G7** and as the
fourth item of the upstream debt, raised there as an issue we never filed.

**Do:** replace the dead `assert` with a real bound and a *counted* refusal
rather than a crash — `sge_oob` beside the ADPCM counters. Then one gameplay
run says whether the 193,693 refusals are reads past the table.

**Cost:** small, and it is a fix plus its own instrument.
**Risk:** low; refusing is what the assert already claims to do.
**NOT established:** that the over-read is a page-table overrun at all. A
constant 11 blocks across buffers of different sizes is not obviously what a
table overrun produces, and that objection is the reason to measure rather
than to patch on the strength of the story.

## 2. Decide read-side vs write-side on the stale buffers *(measurement)*

"The guest is not refilling this buffer" is the instrument's wording and it is
**one of two readings**. The classifier hashes what *we* read, so unchanged
contents mean either the guest never wrote, or its writes are not reaching the
memory we read. This tree has paid for that distinction once already — the
13.5M lost MMIO write windows in `CLAUDE.md`.

**Do:** `RECOMP_APU_WRITE_TRACE` is already armed in `paths.conf`. One
gameplay run, then compare guest writes to the buffers against the freshness
census. No build.

**Why it ranks here:** it decides whether G1 is a guest-side stall or a lost
write, and those have nothing in common.

## 3. One gameplay run answers three questions at once

The next run with a player at the controls should carry all of these; none of
them needs a build, and they are independent:

| arm | answers |
|---|---|
| `RECOMP_APU_WRITE_TRACE` (armed) | item 2 |
| ADPCM ring now prints `bs`, `ba`, `ebo`, `ba_blocks` | base-too-high vs buffer-short |
| `RECOMP_VOICE_RATES_ROWS=256` | whether the music moved to a voice the 12-row table could never show |
| `RECOMP_FB_DUMP` + `RECOMP_REPORT_MS=5000` | more glyph frames; the defect is in ~40% of dialogue boxes |

**Before that run:** empty `glyphdump/` or re-point it — the preserved capture
in `glyphdump-2026-09-19-KEEP/` is safe, but a fresh run overwrites the live
directory.

## 4. G2 needs the font atlas offset, not a cleverer trap

`RECOMP_FF_BATCH_WATCH_TEX` is fixed and now keys per batch rather than per
vertex count (14,273 spurious changes → 145). Its discriminator is finally
usable: batches changing means the vertex data is wrong *before* the shader;
nothing changing while a box renders corrupt means the defect is after the
fetch.

**Blocked on one number.** `01737000` in `paths.conf` was never drawn in four
runs here. Offsets *are* stable across runs (75 of 75 identical), so harvest
the real one with `RECOMP_FF_BATCH_DUMP` in a run that reaches dialogue and
read `tex0=` off a six-vertex text batch.

## 5. The regeneration decision *(the one real fork)*

**G19 and the SHRD fix are in the tool and not in the title.** Both need a
regeneration to reach a running binary, and `CLAUDE.md` warns regeneration is
not bit-stable across translator changes — so it re-bases every comparison,
including the 22:25 / 22:30 / black-screen trio.

SHRD is the one that will change behaviour: `shrd eax, edx, cl` runs twice in
`.text` and a zero count returned `dst | src` instead of leaving `dst` alone.

**Recommendation:** take the healthy control run (items 2–3) *first*, on the
unchanged bundle, then regenerate once and take everything together.

---

# Can we contribute to xboxrecomp? Yes, and the path is established

`origin` is not a fork; upstream merges our PRs. **PR #57 is merged and is an
ancestor of `upstream/main`.** Five more are open (#67, #69, #70, #71, #72),
and the maintainer has been quiet since 16 Sep.

## Three genuinely new PRs from today's work

Checked against `upstream/main` rather than assumed — upstream has **none** of
these:

1. **SHLD/SHRD ignore x86's count rules.** Upstream still emits
   `(dst >> cnt) | (src << (32 - cnt))`: no 5-bit mask, and a count of zero
   shifts by the full width, which C leaves undefined and which returns
   `dst | src` on ARM64 where x86 does nothing at all. **This is the strongest
   candidate** — it is a live wrong value, not a latent one, and
   `_lift_shift` already states the rule that this pair never got. Swept
   against a 64-bit reference over every count 0–40 with a negative control.
2. **`movsd` is two instructions and the dispatcher picks by name.** The
   string branch precedes the SSE branch, so an SSE `movsd` is lifted as a
   string copy touching neither named operand. Latent in JSRF (all 143 are the
   string form); not latent in any title that uses SSE2 doubles.
3. **G19 — the result-setter family rebuilds conditions at the consumer**,
   re-reading a destination a `mov` may have clobbered. Upstream has no
   `_result_snapshot` at all. Larger than the other two and it changes
   generated code everywhere, so it is the one most likely to need discussion
   rather than a merge.

Each already has the thing that got #57 merged: a test that fails when the fix
is reverted.

## One issue to file, not a patch

**G7, `get_data_ptr`'s dead bound.** This was already ranked fourth in the
upstream debt and never filed. File it now — we have evidence it may be live
(item 1) rather than only a code smell.

## Do NOT re-send

**`popfd` is already PR #72.** Today's local fix is the *application* of
something reported on 18 Sep and never merged down into our own `main`; the
discovery was not new.

## And merge upstream down

We are **80 commits behind `upstream/main`**, which has released v0.10.0 and
merged ~20 PRs — **43 of those commits touch `tools/recomp`**, the same
translator today's work is in. That includes our own contributed fixes coming
back the long way.

**Risk is real and specific:** conflicts with the G19 and SHRD changes, and
the possibility that upstream fixed something differently. Merge on a branch,
run all three suites (205 translator tests, 70 ctest, switch audit), and only
then fast-forward `main`.

---

## Rules this session paid for

- **In a per-voice report, `head`/`tail` select a voice, not a time.** Two
  false conclusions in one hour — "`VOICE_FRESH` is measuring nothing" and
  "the `APU-ADPCM` summary counter is dead" — both from comparing one voice's
  row against another's, or an early summary against a late detail.
- **Check whether the instrument is armed AND whether anyone read it.** Four
  instruments this session were armed and unread; three of them had already
  printed the answer.
- **A capped list must say what it dropped.** Three separate tables silently
  truncated: 12 voices of 256, 8 of the failing ADPCM voices, and the FF
  watch's slots.
