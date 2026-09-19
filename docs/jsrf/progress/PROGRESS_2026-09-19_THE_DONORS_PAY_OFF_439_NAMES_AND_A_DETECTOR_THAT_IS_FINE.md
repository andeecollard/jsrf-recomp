# The donors pay off: 439 names, and a detector that turns out to be fine

19 September 2026. Follow-on from extracting all four BC donors. No run.

## 1. Name recovery — 162 → 439

`tools/fusion/signatures.py` has been in the tree unusable, because `build`
takes `<module.dll> <source.xbe>` and nobody had a donor XBE. All four now
exist, so it was run for the first time.

| donor | XDK | signatures built | named in JSRF |
|---|---|---:|---:|
| Fuzion Frenzy | 3911 / 3925 | 1,482 | 231 |
| Blinx | 4831 | 1,786 | 235 |
| Crimson Skies | 5659 | 3,106 | 233 |
| Conker | 5849 | 3,228 | 205 |
| **union** | | | **313** |

**0 ambiguous matches** from any donor. 313 addresses, **308 distinct names** —
only four names land on more than one address, and those are the shared-body
thunk pattern the coverage oracle also sees.

Merged with the earlier size-sequence set: **439 names, 4.9% of 8,876
functions**, against 162 (1.8%) before.

### Quality, measured rather than asserted

- **Cross-donor agreement 93.8%** (226 of 241 addresses named by two or more).
  The 15 "disagreements" are almost all the *same* function differently
  decorated across XDK builds — `?MapTransfer@CMcpxCore@@` against
  `?MapTransfer@CMcpxCore@DirectSound@@`. Not contradictions.
- **Against the older technique**: of 36 addresses both name, they agree on 26
  and differ on 10. The size-sequence file documents its own error rate; the
  byte signatures win in the merge.

### And XDK distance mattered far less than expected

Fuzion (~210 builds away) named 231; Blinx (697) named 235; Crimson (1,525)
named 233. The nearest donor was **not** the most productive. Whatever this
matcher finds is XDK library code that did not change across those builds at
all, so it transfers regardless. That is a useful negative result: the donor
does not need to be close, and using all four beats picking one.

It does not overturn the CLAUDE.md guidance — Fuzion is still the right
*default* on build distance — but the union is what matters, not the choice.

### What it named

57 of 313 are audio, including **29 `CMcpx` and 6 `CAc97`** — the miniport
layer both open audio defects sit in: `CMcpxCore::MapTransfer`,
`CAc97Device::CodecReady`, `CMcpxEPDspManager::AC3GetProgram`.

Files: `~/Library/Application Support/JSRF/jsrf_guest_names_merged.tsv`
(439 entries, same format as the existing file — **not** swapped in; point
`RECOMP_GUEST_NAMES` at it to try). Donor XBEs and signature databases kept in
`~/Library/Application Support/JSRF/bc-donors-2026-09-19/` (58 MB) so this
never has to be re-derived.

## 2. The detector is not the bottleneck

97.6–99.1% agreement with Microsoft's ground truth across four titles, on bare
runs with no seed files.

**This should change a priority.** Adoption-plan item 5, *"keep the sweep in
phase at source"*, was the standing answer to function-boundary weakness.
There is no large weakness to fix. Item 1 (flat dispatch, an entry point per
basic block) still stands on its own merits for indirect calls — but not on
the argument that boundary detection is unreliable.

**The honest limit of that claim:** the oracle only grades against MS-named
*library* functions. The game's own code is not graded by it at all, and JSRF's
8,876 functions are mostly game code. "The detector is good" is established for
XDK library code and assumed elsewhere.

## 3. What this unlocks that has not been done

- **`map_entry_points` / the `(guest, host)` pair arrays.** With guest bytes,
  every mapped address resolves to real guest instructions — so
  `ms-fusion-recompiler.md` §3 can be done systematically instead of on one
  worked example, and §5's `Pri` vs `Fb` split can be correlated with guest
  properties. `Fb` is where Microsoft's static prover gave up; knowing what
  defeats it is the same question as our unresolved stubs.
- **A same-source pair.** `ms-fusion-codegen-corpus.md` lists "whether codegen
  differs between builds `20F90F` and `20F919`" as not established, because
  the corpus had no same-source pair and same-name/same-size was rejected as a
  substitute. With four guest XBEs you can find functions whose *bytes* are
  identical across two donors and settle it.
