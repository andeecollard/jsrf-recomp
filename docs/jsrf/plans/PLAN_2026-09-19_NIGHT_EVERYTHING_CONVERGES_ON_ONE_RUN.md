# Plan, 19 September 2026 (night) — everything converges on one run

Supersedes `PLAN_2026-09-19_THE_AUDIO_PATH_AND_THE_UPSTREAM_DEBT.md`, whose
upstream section is now done (PRs #75/#76/#77) and whose BC section was
overtaken by extracting all four donors.

## The shape of it

Four open questions — the stale buffers, the ADPCM over-read, the glyph
defect, and where the music went — **all need the same thing: one gameplay
session with instruments armed.** None of them needs a different run from the
others. So the plan is: get the bundle ready, take one good run, then spend a
while reading it.

**The constraint that was protecting the old bundle is already spent.** The
player's `JSRF.app` is from 18 Sep 19:58 and contains none of today's
instruments — `RECOMP_VOICE_RATES_ROWS` appears 0 times in it against 3 in
`build-feav`. There is no version of the next run that uses today's work
without rebuilding the bundle. That removes the reason to defer, and it also
sequences the regeneration (see 6).

---

## Before the run — no player needed, all of it mine to do

### 1. G7: the dead bound in `get_data_ptr` *(fix, small)*

```c
unsigned int entry = addr / TARGET_PAGE_SIZE;
assert(entry <= max_sge);          // both call sites pass 0xFFFFFFFF
```

The bound can never fire, so a read past the scatter-gather page table
returns a translation built from whatever dword follows it. The ADPCM block
fetch is one of the two call sites, and the failure signature is a read past a
boundary returning filler.

Replace the dead assert with a real bound and a **counted refusal** — an
`sge_oob` counter beside the ADPCM ones, not a crash. Then the run says
whether the 193,693 refusals are page-table overruns.

**Not established:** that they are. A constant 11-block gap across buffers of
different sizes is not obviously what a table overrun produces, and that
objection is the reason to count rather than to patch on the story.

### 2. Rebuild the player bundle

Required for anything below to produce data. Carries: the voice census and
`RECOMP_VOICE_RATES_ROWS`, the ADPCM `bs`/`ba`/`ebo` ring fields, the re-keyed
FF batch watch, the `AFTER` discard counter, the two newly-printed counters,
and G7 if 1 lands first.

**Generated C unchanged** — engine only. That keeps the new run's guest
behaviour comparable with the 18/19 Sep captures, which a regeneration would
not.

### 3. Prepare `paths.conf`

- `RECOMP_FB_WATCH=150,346,360,28` — the current rectangle is at y=410–444 and
  the dialogue text is at y≈348–396. It has never been on the text.
- `RECOMP_VOICE_RATES_ROWS=256` — or the 2D bin, which is where the music is,
  cannot appear at all.
- `RECOMP_APU_WRITE_TRACE` — already armed; confirm.
- `RECOMP_FF_BATCH_DUMP=400` — to harvest the font atlas offset.
- **Empty `glyphdump/`.** The preserved capture is safe in
  `glyphdump-2026-09-19-KEEP/`, but the live directory is overwritten.

---

## The run — one session, five questions

Play to gameplay, reach dialogue, keep going until the music dies if it will.

| question | what answers it |
|---|---|
| Is the guest failing to refill, or are its writes not reaching us? | `APU_WRITE_TRACE` against the freshness census |
| ADPCM: base too high, or buffer shorter than `ebo` says? | `ba_blocks` on the failure ring |
| Did the music move to a voice the 12-row table could never show? | `VOICE_RATES_ROWS=256` |
| Glyph: wrong before the shader, or after the fetch? | the re-keyed FF watch, once pointed at the real atlas |
| Are the ADPCM refusals page-table overruns? | `sge_oob` from item 1 |

**Harvest the atlas offset first.** `01737000` in `paths.conf` was never drawn
in four runs here. Read `tex0=` off a six-vertex text batch in the
`FF_BATCH_DUMP` output, then re-run with `FF_BATCH_WATCH_TEX` set to it.
Offsets are stable across runs (75 of 75 identical), so this survives.

---

## After the run

### 4. Read it, then decide the audio mechanism

Both audio findings are one measurement from a mechanism. Neither should get
a fix before that measurement.

### 5. G2, with names this time

439 names are now available, 57 of them audio and 29 `CMcpx`. The glyph work
is graphics, and the same signature run named `D3DDevice_` and `D3DX` entries
— worth re-reading `nv2a_pb_exec.c`'s neighbourhood with them loaded.

### 6. Regenerate once, carrying everything

G19 and the SHRD count fix are **in the tool and not in the title**. They need
a regeneration, which is not bit-stable and re-bases every comparison.

**Do it after the run, not before.** The run is worth more against the
existing gen, because the 18/19 Sep captures are the only comparison class we
have. Then regenerate once and carry G19, SHRD, `sal`, `popfd` and `movsd`
together.

---

## Independent tracks — no run, no ordering

### 7. Merge `upstream/main` *(on a branch)*

80 commits behind, v0.10.0, **43 of them in `tools/recomp`** — the same
translator this session edited. Real conflict risk with G19 and SHRD, and
upstream has built its own cross-block comparison-snapshot merge which may
collide with ours. Branch, run all three suites (205 translator tests, 70
ctest, switch audit), only then move `main`.

Worth taking: the function-identification fixes, `preserve RHW in
pretransformed vertex shading` (RHW is how 2D text is drawn — possibly G2),
the darwin compat additions, and our own merged PRs coming back.

### 8. Offer upstream an SVOD extractor

`tools/fusion/coverage_oracle.py` is in their tree and has **never been
runnable by anyone**, for the same reason it was not runnable here: it needs a
donor's guest XBE and the BC packages look sealed. A documented extractor plus
the per-title sector-base derivation turns a dead tool live. That is a better
contribution than another lifter fix, and there are already eight of those
open.

**Not** a prebuilt name database — the tooling is white-room by construction
(signature bytes come from the guest XBE the user owns), but shipping
Microsoft's names is a different question and is the user's to answer.

### 9. BC follow-ons, cheapest first

- **`Pri` vs `Fb`.** `Fb` is where Microsoft's static prover gave up. With
  guest bytes you can ask what those functions have in common — the same
  question as our unresolved stubs.
- **The same-source pair.** `ms-fusion-codegen-corpus.md` lists "whether
  codegen differs between builds `20F90F` and `20F919`" as not established
  because the corpus had no same-source pair. Four guest XBEs can settle it by
  finding functions whose *bytes* match across two donors.

---

## What I would drop

- **Chasing the donor choice.** Measured: Fuzion (~210 builds away) named 231,
  Blinx (697) named 235, Crimson (1,525) named 233. Distance barely mattered
  and the union beats any single donor. Use all four; stop optimising this.
- **Adoption-plan item 5, "keep the sweep in phase at source."** It was the
  standing answer to function-boundary weakness and there is no large weakness
  — 97.6–99.1% across four titles. Item 1 still stands for indirect calls.
  Caveat kept: the oracle grades only MS-named library functions, so this is
  established for library code and assumed for game code.
