> **Filed in `docs/jsrf/` deliberately.** This corrects
> `docs/technical/ms-fusion-recompiler.md`, which is upstream's file, and a
> merge of `upstream/main` is in flight. The corrections below are candidates
> for an upstream doc PR once that lands — do not edit `docs/technical/`
> until it does.
>
> **Not independently re-derived by the session that filed it.** The method is
> corroborated by reproducing the doc's own published numbers exactly
> (Pri 11,118 / Fb 119,935 map entries and the §5 delta histograms), and the
> x86-side claim was cross-checked against our own Crimson disassembly
> (14,217 functions detected; 14,205 starts in the mapped span, all present in
> Microsoft's map). The PPC-side statistics have not been recomputed by hand.

# What defeats Microsoft's static recompiler: `Pri` vs `Fb` against the guest code

Read-only analysis, 19 Sep 2026. Scripts in `prifb/`, raw output in `prifb/report*.txt`.

---

## 0. Headline

**Nothing in the guest code selects a function into `Fb`, because `Fb` takes every
function.** `Fb` covers 100.000 % of the instruction words of all 1,532 functions in the
module — `Fb`-entries/word has zero variance across functions (min = max = 1.0000). There is
no per-function fallback to characterise, so none of the candidate discriminators can
separate the tiers, and I measured that they do not.

What the two maps actually differ on is something else, and it is crisp:

> **A `Pri` map entry exists exactly where the runtime can *name* a guest address through the
> ABI — a function entry, or the return address after a call.** Per function, the number of
> `Pri` entries equals `1 + (number of call instructions)` **exactly** for 1,385 of 1,532
> functions (90.4 %), r = 0.9974. Everything else — every branch target, every basic-block
> head inside a function — is resolved at build time and gets no entry.

So the tier split is not "what the analysis could prove". Both tiers translate the same code.
`Pri` omits entries for addresses that *cannot arise at runtime*; `Fb` emits one for every
word so that it can be entered anywhere.

The only guest-code property that genuinely separates them is **non-code bytes sitting in an
executable section** (~14 KB, 2.2 % of the guest code span), where `Pri` emits nothing and
`Fb` blindly translates data as instructions.

---

## 1. What was measured, and on what

**Title:** Crimson Skies: High Road to Revenge BC package.
**Module pair:** `Content/xeo3_5fb3687c_001748c4.dll` (`Pri`) and `…_no.dll` (`Fb`).
**Guest:** `Content/Flash/xboxkrnlcf.bin` — the Xbox 360 kernel.

The guest is a plain, unencrypted PowerPC PE (`Machine 0x01F2`, `ImageBase 0x80040000`).
One correction worth recording: **it is a flat memory image — file offset == RVA**, not a
`PointerToRawData` layout. `SizeOfImage` == file size == `0x170000`, and `.xedata` at
`VA 0x10000` / `PointerToRawData 0xE600` only decodes under the VA reading. Reading it the
normal PE way yields zeros for `.text`.

| | |
|---|---|
| `.text` | RVA `0x11000`, `0x76450` bytes = **121,108 PPC words** |
| `.pdata` | 1,532 function records, 423,400 bytes = 87.4 % of `.text`, no overlaps |
| `.xedata` | RVA `0x10000`, `0xEB8`, marked executable |

`.pdata` bitfield packing is MSVC little-endian-order inside the big-endian word
(`prolog:8 | length:22 | thirty_two_bit:1 | exception_flag:1`), i.e. `len = (v>>8) & 0x3FFFFF`
in **words**, as xenia reads it. Reading it MSB-first gives nonsense (constant prolog `0x40`).

### The doc's method reproduces exactly

`docs/technical/ms-fusion-recompiler.md`'s appendix is correct. The two `lea`s in
`InitPrecompiledDll` gave `Pri` `0x11D6F0 .. 0x133260` and `Fb` `0x2B39D8 .. 0x39DDD0`,
the bounds the doc states, yielding:

| | `Pri` | `Fb` |
|---|---:|---:|
| pair entries | **11,118** | **119,935** |
| guest delta histogram | 8 (×2130), 16 (×1111), 12 (×853), 24 (×789), 20 (×748), 28 (×607) | **4 (×118,373)**, 8 (×1337), 12 (×206) |

Identical to §5 of the doc, delta histogram included. Entry 0 of each array is a `(0,0)`
sentinel; all counts below use the remaining 11,117 / 119,934.

### Coverage, word granularity, over the 121,108 words of `.text`

| | words | % of `.text` |
|---|---:|---:|
| `Pri` entry | 11,117 | 9.18 % |
| `Fb` entry | 119,002 | 98.26 % |
| `Pri \ Fb` | **0** | — |
| `Fb \ Pri` | 107,885 | 89.1 % |
| neither tier | 2,106 | 1.74 % |

**`Pri` is a strict subset of `Fb`.** Plus `.xedata`: `Fb` 932 entries, `Pri` **0**.

---

## 2. What a `Pri` entry is (denominator 11,117)

| class of guest word | words | with a `Pri` entry | % of class | % of all `Pri` |
|---|---:|---:|---:|---:|
| `.pdata` function start | 1,532 | 1,532 | **100.0 %** | 13.8 % |
| direct call (`bl`) target | 1,549 | 1,549 | **100.0 %** | 13.9 % |
| return address (word after any call) | 8,761 | 8,620 | **98.4 %** | 77.5 % |
| **CORE = union of the three** | **10,789** | **10,648** | **98.7 %** | **95.8 %** |
| direct jump (`b`) target | 2,477 | 501 | 20.2 % | 4.5 % |
| conditional branch (`bc`) target | 8,401 | 825 | 9.8 % | 7.4 % |
| fallthrough after a non-call branch | 17,226 | 1,401 | 8.1 % | 12.6 % |
| **branch target that is *not* CORE** | **9,084** | **39** | **0.4 %** | — |

The last row is the whole story: **a guest address that is only ever reached by a direct
branch has a 0.4 % chance of a `Pri` entry; one that can be reached by a call or a return has
a 98.7 % chance.** 230× separation (39/9,084 = 0.43 % against 10,648/10,789 = 98.7 %).

Per-function regression: `Pri entries == 1 + call instructions` exactly in **1,385 / 1,532
(90.4 %)**; 136 functions below (calls to no-return callees / tail calls), 11 above
(extra indirectly-entered addresses). r(`Pri`, `1+calls`) = **0.9974**;
r(`Pri`, function size) = 0.8335. Totals: 10,190 `Pri` entries inside functions against
10,284 predicted.

469 `Pri` entries (4.2 %) fall outside CORE. 92 of those fall outside *every* direct-CFG
class; 82 of the 92 are preceded by a zero padding word and 14 appear as pointers in guest
data — i.e. they are function entry points that `.pdata` does not describe.

---

## 3. Discriminators tested that do **NOT** separate the tiers

Function level, n = 1,532. `Pri`-residual = `pri − 1 − calls`, which strips out the
call-density effect. **`Fb`-entries/word is 1.0000 in every single cell of this table.**

| candidate | n | `Fb`/word | `Pri`/word (in vs out) | `Pri` residual (in vs out) | verdict |
|---|---:|---|---|---|---|
| contains `bctr`/`bctrl`/`blrl` (indirect jump or call) | 95 | 1.0000 vs 1.0000 | 0.1210 vs 0.1228 | +0.02 vs −0.07 | **no effect** |
| has a `.pdata` exception handler | 17 | 1.0000 vs 1.0000 | 0.1625 vs 0.1223 | **+1.12** vs −0.07 | **opposite** — SEH functions get an *extra* `Pri` entry (the handler address), they are not pushed to `Fb` |
| contains `sc` (syscall) | 1 | 1.0000 | 0.0536 | +4.00 | n too small |
| ≥ 1 KB | 39 | 1.0000 vs 1.0000 | 0.0680 vs 0.1242 | −0.13 vs −0.06 | apparent size effect is **entirely** call density; nothing left after control |
| ≥ 2 KB | 5 | 1.0000 vs 1.0000 | 0.0616 vs 0.1229 | −0.60 vs −0.06 | same |
| leaf (no calls) | 33 | 1.0000 vs 1.0000 | 0.2210 vs 0.1206 | +1.21 vs −0.09 | artefact of 1 entry ÷ small size |
| ≥1 word capstone cannot decode (VMX128) | 14 | 1.0000 | — | — | **no effect** |
| **functions with 0 `Pri` entries** | **0** | — | — | — | there is no `Fb`-only function |

**Reached only through a data table — also negative.** Scanning every non-`.text` section for
words that are valid in-image pointers to `.text` gives 1,617 distinct targets
(`.rdata` 176, `.pdata` 1,532, `.data` 33, `.edata` 1). **1,576 (97.5 %) have a `Pri` entry**;
0 have no translation at all. Restricting to the hard case — a target that is neither a
`.pdata` start nor ever a direct `bl` target — leaves 85 addresses, of which 44 (51.8 %) still
have a `Pri` entry. Separately, `.xedata` holds a table of 791 distinct bare `.text` RVAs;
**790 of 791 (99.9 %)** have a `Pri` entry. Microsoft resolved the tables.

**Self-modifying / overlapping code — untested, not refuted.** No overlapping `.pdata` ranges
and no SMC evidence in this module, so there were no instances to measure.

---

## 4. What *does* separate them

### (a) Non-code bytes inside an executable section — the only real `Fb`-only regions

**The `.text` tail, `0x084BD0 .. 0x087450`** — 2,592 words, 10,368 bytes, **8.6 % of `.text`**,
beyond the last `.pdata` function:

| | |
|---|---|
| `Pri` entries | **0** (the last `Pri` entry in the whole module is at `0x084BA8`) |
| `Fb` entries | 1,597, spanning **45,308 host bytes** (17.5 B/word) |
| capstone-decodable as PPC | 245 / 2,592 |
| word content | primary opcode field is 0 (`00c60400`, `007e424b`, `0098c007` …) — not PowerPC |

`Pri` almost certainly emitted no host code there either: the `Pri` map's last host RVA is
`0x11577D` and `Pri`'s `.text` is 1,142,390 bytes, leaving **at most ~5.9 KB** of host code
after the last mapped guest address, against the ~18 KB that 2,592 words would need at `Pri`'s
measured 7 B/word. So **`Pri` correctly classified 10 KB of data as data; `Fb` translated it
as instructions.**

**`.xedata`** (RVA `0x10000`, `0xEB8`, `Characteristics 0x60000020` = executable): `Pri` **0**
entries, `Fb` **932**. Its content is the XEX metadata blob — a table of `.text` RVAs, not
code. Same pattern.

Combined: ~2,529 words / ~14 KB where the tiers disagree because one of them is wrong about
code-vs-data, and it is `Fb`.

### (b) `Fb`'s emission rule is near-syntactic

Testing `Fb entry ⟺ word ≠ 0` over all 121,108 words:

| | nonzero word | zero word |
|---|---:|---:|
| `Fb` entry | 118,871 | 131 |
| no `Fb` entry | 65 | 2,041 |

**99.838 % agreement**, and **100.000 % inside `.pdata` functions**. The rule has exactly two
kinds of exception:

- the 131 zero words it *did* emit for are all inside the data tail (`0x085058`…);
- the 65 nonzero words it skipped are **15 inline-data islands**:
  - 14 islands of 1–2 words, every one an in-image pointer (`0x80073000` → `.text+0x22000`,
    paired with a `.rdata` pointer `0x80045Cxx`), sitting immediately before a function
    prologue (`mflr r12`). Both tiers skipped all of them — **the compiler's code/data
    separation is accurate to the word** on inline data.
  - one 39-word island at `0x0397F4` which *is* valid PowerPC (a `memset` loop:
    `mtctr r0 / ori / b / stb / stw…`) that `Fb` skipped anyway.

### (c) The "neither tier" set is padding, not defeated code

2,106 words (1.74 % of `.text`):

- **2,041 (96.9 %) are all-zero words**; only 65 are nonzero (the islands above);
- run lengths: 1,334 singles, 206 pairs, 12 triples, one ×5, one ×39, one ×280;
- **0 of them lie inside a `.pdata` function.**

It is inter-function alignment padding. There is essentially no interleaved data in this
module's function bodies.

---

## 5. Regions with no `Pri` entry are still translated by `Pri`

575 contiguous runs hold zero `Pri` entries, 38,362 words = **31.7 % of `.text`**. That is not
untranslated code. Using the map's *host* column to measure how much host code spans each
region:

| region | words | `Pri` host B/word | `Fb` host B/word | incoming direct branches from outside | calls inside |
|---|---:|---:|---:|---:|---:|
| `0x031184` memcpy/memcmp family | 1,171 | **6.5** | 23.1 | 32 | 0 |
| `0x053CC8` FP + VMX128 routine | 1,040 | **16.3** | 50.5 | 2 | 0 |
| `0x0544EC` byte-identical duplicate of it | 519 | **16.2** | 50.5 | — | 0 |
| `0x039314` MSVC save/restore-GPR helper block | 411 | **8.0** | 25.1 | **398** | 0 |
| `0x084BD0` the data tail | 2,592 | **0** | 17.5 | 1 | 1 |

Module-wide medians: `Pri` **10.5** host bytes per guest word, `Fb` **20.0**. Every zero-`Pri`
code region is translated at normal `Pri` density. They have no entry because they contain no
call and are entered only by direct branch — there is no runtime-suppliable address inside
them. The data tail is the sole region where `Pri` emits nothing at all.

Inside vs outside `.pdata`:

| | words | `Pri` | `Fb` | neither | call instructions |
|---|---:|---:|---:|---:|---:|
| inside `.pdata` | 105,850 | 10,190 (9.63 %) | 105,850 (**100.00 %**) | 0 | 8,752 |
| outside `.pdata` | 15,258 | 927 (6.08 %) | 13,152 (86.20 %) | 2,106 | 9 |

Note the 927 `Pri` entries outside `.pdata` against only 9 calls there: outside the unwind
table, `Pri` entries are entry points (thunks, address-taken stubs, helper-block offsets), not
return addresses.

---

## 6. The x86 layer — our own guest architecture

**There is no fallback tier on x86 anywhere.** Across all four packages: 2 `xefu_*` modules
each and **0** `xefu_*_no.dll`; 9 `xeo3_*` `Pri`/`Fb` pairs each. Confirmed by enumeration.

Crimson's x86 game module (`xefu_f4dc7aa0_…`) map bounds read from `InitPrecompiledDll` are
`0x1457190 .. 0x27C4FD8` — exactly the bounds the doc quotes. 2,546,633 pairs over guest
`0x1000 .. 0x27E466`:

- covered **2,546,633 of 2,610,279 bytes = 97.56 %**; uncovered 63,646 B = 2.44 %
- 54,626 uncovered runs: **47,377 of length 1**, 5,934 of length 2, 1,163 of length 3,
  117 of length 4, and a tail to length 37
- **the largest uncovered run in the entire 2.6 MB module is 37 bytes**

So the x86 tier has no large unreachable region — no analogue of the PPC data tail. The
byte-dense map plus recovery stubs really does subsume what `Fb` does on PowerPC.

Against our own disassembly of the same guest (`crimson_disasm/functions.json`, 14,217
functions):

- **14,205 / 14,205 (100 %)** of our function starts inside the span have a Microsoft map
  entry — our function detection finds nothing Microsoft missed;
- **163,492 bytes (6.42 % of Microsoft's mapped span) lie in no function we detected** —
  Microsoft translates 163 KB of this title that our function-boundary pass does not claim;
- 61,552 bytes inside our functions have no Microsoft entry (2.518 % of our function bytes),
  and 96.7 % of all uncovered bytes are inside one of our functions — the gaps are interior
  1–3 byte holes, not whole regions.

---

## 7. What I could not determine

- **The other five PowerPC module pairs.** `xam.xex`, `hud.xex`, `ximecore.xex`, `xefu.xex`,
  `xefutitle.xex` are all `XEX2` containers (compressed and/or encrypted); their guest code is
  not readable here. Every guest-side claim above rests on `xboxkrnlcf.bin` alone.
  `xboxkrnlcf.bin` is the only plain-PE guest in the PowerPC layer.
- **Whether `Pri` emits host code for the data tail and `.xedata`.** Bounded above at ~5.9 KB
  from the host-column arithmetic, but a host region with no map entry cannot be attributed
  from the map alone.
- **Which tier the runtime selects, and when.** Nothing in either module records a selection
  policy. The doc's reading (exceptions, debugger, untraced indirect targets, SMC) is a
  reading; I did not measure it, and nothing here confirms or contradicts it.
- **What the 10 KB tail blob is**, beyond "not PowerPC code", and what `.xedata`'s RVA table
  is used for beyond "a list of `.text` addresses".
- **Why `Fb` skipped the valid 39-word `memset` at `0x0397F4`** — unreachable-code elimination
  or misclassification; both are consistent with the data.
- **Self-modifying and overlapping code as discriminators**: no instances in this module, so
  untested rather than refuted.

---

## 8. Bearing on our recompiler

The cost estimate for an interpreter fallback should go **down**, because a fallback is not
what buys Microsoft its correctness here.

1. **`Fb` is not triage.** It is a complete second translation of the entire module —
   2.45× the `.text`, 10.8× the map — with no per-function selection. Building our own `Fb`
   means building a second whole backend, and it would buy only the ability to *enter* at an
   arbitrary address. `docs/technical/ms-fusion-adoption-plan.md` marks the interpreter
   fallback "deferred (disproportionate)"; that judgement holds, and for a sharper reason than
   before: the fallback is not where the unresolved-address problem is solved.
2. **`Pri`'s entry set is small, mechanically derivable, and needs no proofs.** Function
   entries plus the word after every call — 10,789 addresses for 121,108 instructions (8.9 %).
   `.pdata` plus a linear scan for call opcodes reproduces 98.7 % of it. No traces, no
   enlightenments database, no fixpoint required for *this* part.
3. **`Pri` does not emit an entry per basic block.** Adoption item 1 in the teardown proposes
   "an entry label at every basic-block head"; that is *more* than Microsoft does on PowerPC,
   where branch-only targets get an entry 0.4 % of the time. The set that matters is call
   targets and return addresses — which is exactly the set our `RECOMP_ICALL` /
   `*_stubs_unresolved.c` path fails on.
4. **Adoption item 2 is load-bearing.** 77.5 % of all `Pri` entries are return addresses. Our
   `PUSH32(esp, 0)` dummy return address is not a corner case in this model; it is the
   single most common thing the address map exists to serve.
5. **Our weak link is function detection, and it is measurable.** Microsoft's x86 map covers
   163 KB of Crimson that `tools/disasm/functions.py` does not place in any function — 6.4 %
   of the code span. On the x86 side Microsoft needs no function boundaries at all; every one
   of our unresolved stubs is a cost of having them.
6. **Code-vs-data in an executable section is the one place their analysis visibly fails**,
   and it fails in the *safe* direction: the fallback tier over-translates 14 KB of data. Our
   equivalent failure — an unresolved stub that adjusts `esp` and returns — fails in the unsafe
   direction.
