# The decompilation's symbol table names our addresses — and names G4's suspect

Date: 2026-09-10 (Europe/London)
Branch: `reference/xemu-oracle`
Tool added: `diagnostics/jsrf_first_fault/symbolize.py`

Supersedes the G4 section of `goals/JSRF_GOALS_2026-09-10_FOUR_FAULTS.md` on the
question of *how* to attack the draw phase. The lead it calls "strongest
untested" is no longer a lead requiring a hunt; the structures have names,
addresses and offsets.

---

## 1. The asset

`../JSRF-Decompilation/ghidra/symboltable.tsv` — KeybadeBlox' matching
decompilation of the same **US** XBE we translate, checked out beside the repo.
1332 named functions with return types, calling conventions and typed
parameters, plus C++ headers carrying real struct layouts.

**It is in our VA space, measured, not assumed:**

```
named functions          : 1332
exact match on our start : 1319  (99.0%)
inside one, not its start:   13
unknown to our disasm    :    0
```

Re-measure with `symbolize.py check` rather than trusting this table.

The 13 are not defects. Twelve are `*_handler` — MSVC SEH funclets, which
legitimately live inside their parent's span — and the thirteenth the
decompilation itself calls `functionAtUnfortunateAddress`.

**Not vendored.** That project publishes no licence and asks that no
language-model output be contributed back to it. `symbolize.py` reads the table
in place from the sibling checkout and writes nothing to it. Take care before
committing any of its data into this repo.

---

## 2. CActMan, with offsets

`decompile/src/JSRF/Action.hpp` defines the manager struct. Offsets below are
computed from the field list, then checked against the struct's own
self-naming fields (`unknown0x4`, `unknown0x87B4`, `unknown0x8838`, …).
**Twelve anchors, zero disagreements.** Total size `0x8840`.

| offset | field | note |
|---|---|---|
| `+0x003C` | `m_bUseFallbackBgColor` | |
| `+0x0040` | `m_bCoveredPause` | paused, world **not** visible |
| `+0x0044` | `m_bEvent` | cutscenes |
| `+0x0048` | `m_bFreezeCam` | |
| `+0x004C` | `m_bUncoveredPause` | paused with world visible, or auto-pause at mission start |
| `+0x0070` | `m_bDrawChildren` | |
| `+0x0074` | `m_bSkipDraw` | |
| `+0x0094` | `m_DrawMode` | `eDRAWMODE_YES / _WAITVBLANK / _NO` |
| `+0x0098` | `m_lpActTbl[7668]` | the object table |
| `+0x7FA4` | `m_lpDrawRoot` | **draw list — never walked** |
| `+0x7FAC` | `m_lpDrawSortRoot` | **sorted draw list — never walked** |
| `+0x7FB4` | `m_lpDrawSortBinRoots[256]` | **256 sort bins — never walked** |
| `+0x87DC` | `m_lpActExecRoot` | the exec tree |
| `+0x87E8` | `m_dwActCount` | |

Two independent confirmations that this table is right:

- **Our own harness already uses two of them.** `main.c:1466-1467` defines
  `JSRF_SCENE_OFF 0x87DC` and `JSRF_LIVE_OFF 0x87E8`, derived by measurement
  months before this table was read. They are `m_lpActExecRoot` and
  `m_dwActCount`.
- **The pause measurement matches semantically.** The 09-10 handover recorded
  that pausing sets `+0x3C` and `+0x40` to 1. Those are
  `m_bUseFallbackBgColor` and `m_bCoveredPause` — and a covered pause is
  defined as the world not being visible, which is exactly when a fallback
  background colour would be wanted. Two fields, one coherent behaviour.

`mode_finder.py`'s "6 noisy dwords out of 8704" was snapshotting this struct:
8704 dwords is 34816 bytes against the struct's 34880.

---

## 3. What this settles for G4

**Our 67-object walk traversed the exec tree, and the draw phase does not use
it.** `JSRF_SCENE_OFF` is `m_lpActExecRoot`. The draw phase walks
`m_lpDrawRoot`, `m_lpDrawSortRoot` and 256 `m_lpDrawSortBinRoots`, maintained by
a separate API:

```
0x000127F0  CActMan::InsertDrawList(CActBase *)
0x00012840  CActMan::DeleteDrawList(CActBase *)
0x00012A00  CActMan::InsertDrawSortList(CActBase *)
0x000129D0  CActMan::initSortDrawList()
0x000131A0  CActMan::sortDrawList()
```

"67 live registered, 67 walked, zero marked dead" is therefore **not** evidence
that the characters reach the renderer. It is evidence about a different data
structure. That finding stands; its scope was narrower than it read.

### The mode system is five-way, not a boolean

Both exec and draw have five variants — `Default`, `Event`, `CoveredPause`,
`FreezeCam`, `UncoveredPause` — dispatched per object
(`CActBase::drawManyDefault`, `drawManyEvent`, `drawManyCoveredPause`,
`drawManyFreezeCam`, `drawManyUncoveredPause`, and `drawTree*` / `recursiveExec*`
for each). `Action.hpp` documents the precedence:

> `coveredPause > Event > FreezeCam > UncoveredPause > Default`

All four booleans zero selects `Default`, which is why all-zero is the normal
reading. That is consistent with what was already measured and adds the reason.

### The per-object filter

```
CActBase::drawManyDefault(eACTFLAG flagFilterAny1, int drawArg1, int drawArg2,
                          eACTFLAG flagFilterAll, uint otherBitfieldFilterAny,
                          eACTFLAG flagFilterNone, eACTFLAG flagFilterAny2)
```

Any/All/None bitmask filters over each object's `eACTFLAG`. An object present in
the draw list but failing a filter is silently not drawn — no cull, no reject,
no missing method. That is precisely the observed symptom.

### Instrumentation targets, in priority order

| address | function | question it answers |
|---|---|---|
| `0x00012580` | `CActMan::drawOne(CActBase*, uint)` | **which objects actually draw** — one call site, from `drawSub` |
| `0x000131F0` | `CActMan::drawSub()` | the walk itself |
| `0x00012C80` | `CActMan::drawMany(eACTFLAG flagFilterAll, BOOL)` | the filter actually applied |
| `0x000125E0` | `CActMan::drawListSub(...)` | list traversal and its filters |
| `0x000127F0` / `0x00012840` | `InsertDrawList` / `DeleteDrawList` | whether Corn was ever **in** the list |
| `0x000127B0` | `CActMan::SetDrawMode(eDRAWMODE)` | called only from `CProgress::Exec0Default` and `CProgress::hide` |
| `0x00012760` | `CActMan::DrawSkip()` | |

`drawOne` is the one to take first: a single call site, one object per call, and
its object set differenced against the 67 from the exec walk answers "is the
character absent from the draw list, or present and filtered out?" — which is
the whole of G4's remaining ambiguity.

---

## 4. The tool

```sh
symbolize.py check                 # agreement with our disasm (re-measures §1)
symbolize.py lookup drawTree       # by name substring, or by 0xADDRESS
symbolize.py callers 0x00012580    # call graph, named on both sides
symbolize.py annotate run.log      # rewrite sub_/0x forms in any log
symbolize.py map --out names.json  # {va: name}
```

`annotate` is the one that compounds: every instrument in this tree prints raw
`sub_XXXXXXXX`, and piping a run through it makes the output readable without
touching the instruments.

---

## 5. Next

1. Instrument `drawOne` and diff its object set against the exec walk's 67, at
   the stuck tutorial. This is the G4 measurement the whole section has been
   waiting for, and it needs no debugger.
2. If the character is absent from the draw list, walk back to
   `InsertDrawList` / `DeleteDrawList`. If present but filtered, read its
   `eACTFLAG` against the filters `drawMany` was called with.
3. Unrelated but now cheap: `Opening::drawDefault` (`0x0007E550`) and
   `Opening::Exec0Default` (`0x0007E360`) are the intro cards' object. G1 asks
   what computes the combiner constant and concluded the title "never calls
   SetRenderState at all". These two functions are where to look.
