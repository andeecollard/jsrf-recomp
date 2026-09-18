# Fission Codegen Across the Corpus — Register Model, Flags, Dispatch, Tiers

Third document in the Ficl/Fission series. [ms-fusion-recompiler.md](ms-fusion-recompiler.md)
established the package layout, the build pipeline and the HLE boundary;
[ms-fusion-codegen-teardown.md](ms-fusion-codegen-teardown.md) established that Microsoft ships
**two** recompiler architectures and that we built the PowerPC-shaped one for an x86 guest;
[ms-fusion-corpus.md](ms-fusion-corpus.md) extended the identity work to four titles. This
document answers the *codegen* questions those three left open, on all four titles plus the
shared kernel and six PowerPC module pairs, and it corrects two things they got wrong.

**No Microsoft code, IR, output or data table is copied, transcribed or paraphrased into this
repository.** Nothing below is a disassembly listing. Every mechanism is described in my own
words from reading publicly distributed retail binaries with `pefile` and `capstone`; the
numbers are counts and measurements I produced, which are facts about those binaries, not
their content. Where an earlier document in this series quoted Microsoft's output directly,
this one does not — only mechanism and measurement.

Subjects: the four shipped BC packages, package version **2608.3123.1.0**, read 18 Sep 2026.
One revision newer than the `2607.x` subjects of the first two documents, which matters —
see §8.

Every claim below is tagged **MEASURED** (a number I produced, reproducible from the appendix)
or **INFERRED** (a reading of those numbers that the binaries do not directly state).

---

## 1. The corpus, and one methodological warning

| module | source | build | `.text` | map entries | guest span | host B / guest B |
|---|---|---|---:|---:|---:|---:|
| `xefu_f4dc7aa0_…` | Crimson `default.xbe` | `20F90F` | 21,322,744 | 2,546,633 | 2,610,279 | 8.17 |
| `xefu_556879e0_…` | Blinx `default.xbe` | `20F912` | 12,423,560 | 1,505,965 | 1,540,875 | 8.06 |
| `xefu_31bef64e_…` | Conker `default.xbe` | `20F917` | 45,854,680 | 5,442,333 | 5,570,535 | 8.23 |
| `xefu_5859e34a_…` | Fuzion `default.xbe` | `20F919` | 19,356,632 | 2,316,973 | 2,368,327 | 8.17 |
| `xefu_69c41281_00027bcf` | `xb1krnl.exe` | `20F912` | 853,544 | 98,270 | 125,503 | 6.80 |

Plus, on the PowerPC layer, the `Pri`/`Fb` pairs for `xam.xex`, `xboxkrnlcf.bin`,
`Xam.Community.xex`, `ximecore.xex`, `hud.xex`, `xefutitle.xex`. (`xefu.xex`'s module,
`xeo3_615ec97d_…`, is deliberately untouched here — it is another investigation's subject.)

All map bounds are read exactly from `InitPrecompiledDll`, not scanned. **MEASURED.**

### The warning: most of the emitted arena is shadow code

The x86 arena has a translation entry point at essentially every guest byte, and the entry
for a byte that is *not* an instruction start is a real translation of the misaligned decode
starting at that byte. So a raw mnemonic histogram over `.text` counts thousands of `sahf`,
`lahf`, `pop ds` and `int3` translations that no correct execution ever reaches — they are
translations of garbage that only a misaligned entry could observe.

**Every instruction-frequency number in this document is a count of emitted host code, not of
hot-path guest work.** Structural idioms (dispatch, helper escape, preemption poll, stack
adjustment) are safe to read either way because nothing but a genuine translation emits them.
Opcode-frequency claims are not. This is stated once and assumed throughout.

---

## 2. Q1 — The register model

**The eight guest GPRs live in host registers, one to one, everywhere, with exactly one
documented seam.** **MEASURED**, on all four titles and the kernel, and it does not vary by
function, by region, or by title.

The allocation is fixed for the whole module:

| guest | host | notes |
|---|---|---|
| `eax ecx edx ebx esi edi ebp` | the identically-named host register | never spilled |
| `esp` | `r14` (32-bit half) | adjusted only with flag-preserving address arithmetic |
| guest RAM base | `r15` | never reloaded, never modified |
| — | `r12` | per-thread host-side emulator block |
| — | `r13` | big-endian emulator/CPU state block (see §3, §6) |
| — | `r8`–`r11` | scratch: computed guest addresses, helper arguments, temporaries |

Evidence that it is invariant: every guest memory access uses one of two addressing shapes —
`[guest-reg + r15 (+ disp)]` or `[r15 + scratch-reg]` — and the guest registers appearing in
the first shape are exactly `eax ecx edx ebx esi edi ebp r14`. In a 3 MB Blinx window,
**207,958 of the 255,693 memory operands that are not address arithmetic (81 %) are
`r15`-based**, and essentially all of the rest is the dispatch table, the host stack, the
module's own pointer block and the two state blocks; unexplained residue is 2.5 %,
concentrated in shadow regions. There is no context block for GPRs anywhere in the x86 layer,
no spill slot, and no reload after an escape to the runtime. **MEASURED.**

### The seam: high-byte registers

x86-64 cannot encode `ah`/`ch`/`dh`/`bh` in any instruction that also carries a REX prefix,
and every guest memory access carries REX (it uses `r15`). Fission's answer is to swap the
wanted high byte into the corresponding low byte, do the work, and swap back — a register-to-
register byte exchange on each side of the access. Counted over whole modules:

| | kernel | Crimson | Blinx | Conker | Fuzion |
|---|---:|---:|---:|---:|---:|
| `dh`↔`dl` swaps | 2,001 | 38,446 | 21,594 | 100,438 | 34,796 |
| `ah`↔`al` swaps | 1,070 | 25,763 | 21,042 | 93,243 | 43,861 |

**MEASURED.** In a decoded window the four high-byte pairs account for ~93 % of all
register-to-register exchanges emitted. This is the only place the 1:1 mapping is not free,
and it costs two extra instructions per affected access. **INFERRED**, but the operand
statistics leave little room for another reading.

### The escape convention keeps the mapping intact

At a runtime escape, nothing is spilled and nothing is reloaded: the guest registers stay
live in their host registers across the call. Arguments go in `r8` (always the guest EIP),
`r9` (a kind code) and a small block at the host stack pointer. **MEASURED** — 108,980 escape
sites in Crimson, every one of them preceded immediately by a guest-EIP load into `r8`, and
none of them followed by a guest-register reload.

So the runtime helpers must preserve `rax rcx rdx rbx rsi rdi rbp r12 r13 r14 r15` — a
non-standard ABI in which almost every register is callee-saved. **INFERRED.** The helper
bodies live in `Emu.exe`, which is ACL-locked, so this cannot be confirmed directly.

### Contrast: the PowerPC layer, measured on a module the teardown never opened

On `xam.xex` (`Pri`, 3 MB window, 700,550 instructions), **398,273 of 484,892
non-address-arithmetic memory operands — 82 % — are displacements off `rbx`**, the guest
context block, against 71,599 `r15`-based operands, which are the guest *data* accesses. There is no attempt at a register mapping. This reproduces the teardown's
finding from `xboxkrnlcf` on a nine-times-larger module. **MEASURED.**

**The answer to "does the register model vary by function" is no, and the more useful answer
is that it varies by guest architecture and by tier.** The `Fb` tier does not even pin the
memory base: it addresses guest memory through ordinary register pairs and reloads the base
from the context block. **MEASURED** (`Fb` windows contain zero `r15`-based operands).

---

## 3. Q2 — Flags

**Eager, free, and inherited — with one flag that cannot be, kept in memory and spliced in
only when the guest looks at it.** **MEASURED.**

Because guest and host share the flags register, there is no flag computation to speak of on
the x86 path. The measurable proxy is how often the compiler has to *materialise* a condition
into a value. Condition-set instructions per million emitted instructions:

| module | layer | setcc / M insns |
|---|---|---:|
| Blinx game | x86 → x64 | 58 |
| Fuzion game | x86 → x64 | 123 |
| Crimson game | x86 → x64 | 189 |
| Conker game | x86 → x64 | 215 |
| `xb1krnl` | x86 → x64 | 175 |
| `xam.xex` `Pri` | PPC → x64 | **96,400** |
| `xam.xex` `Fb` | PPC → x64 | **54,120** |

A factor of **500–1,600×**. **MEASURED.** That single table is the whole story: on the PPC
path roughly one instruction in ten is materialising a condition bit, because PowerPC
condition registers have no host equivalent; on the x86 path it is statistical noise. The
teardown's observation that Microsoft decomposes `CR0` into separate bytes *in the optimized
tier* is confirmed, and now quantified, on a second and much larger module.

### Flag preservation, not flag computation, is what the x86 path spends effort on

Three mechanisms, all **MEASURED**:

1. **Flag-preserving address arithmetic for every stack adjustment.** In one 3 MB window of
   the Blinx module (732,277 instructions), 60,239 address-computation instructions target the
   guest stack register and nothing else — every guest push/pop adjusts `esp` without touching flags.
2. **`inc`/`dec` in preference to add/subtract by one** where the guest semantics allow it
   (15,855 / 6,266 in the same window on guest registers), which preserves the carry flag.
3. **Address arithmetic substituted for guest arithmetic when the flags are dead** — 822
   cases in that same window where a guest add-immediate into a register became a flag-preserving
   address computation into the same register. This is a genuine dead-flag analysis, not a
   peephole: the substitution only appears where nothing downstream reads the flags.
   **INFERRED** from the shape, but the alternative (that it is unconditional) is refuted by
   the much larger count of ordinary flag-setting adds.

### The interrupt flag is the one exception, and it is elegant

Guest `pushfd` does not simply push the host flags. It captures the host flags, loads a
32-bit field from the big-endian emulator state block, isolates bit 9 (`IF`) from it, splices
that bit into the captured value, and pushes the result. So every arithmetic and status flag
is the host's, and the one flag the host cannot represent for a guest — interrupt-enable — is
shadowed in memory and merged in only at the moment the guest observes `EFLAGS`. Counted
whole-module: 2,476 (Crimson), 1,323 (Blinx), 7,542 (Conker), 3,299 (Fuzion), 97 (kernel)
sites. **MEASURED.**

Guest `popfd` is the mirror: pull a dword off the guest stack, adjust the guest stack
pointer, and load the host flags from it (1,404 sites in one Blinx window, with a completely
uniform three-instruction prefix). **MEASURED.**

**There is no evidence of lazy flag evaluation anywhere, and none is possible in this design:**
the host instruction that implements the guest instruction computes the guest's flags as a
side effect, at zero cost. Flag *outcome materialisation* — the thing the teardown told us to
steal — is a PowerPC-path technique, and on the x86 path it is correctly absent. **INFERRED.**

---

## 4. Q3 — Dispatch

**One flat table, indexed by the raw 32-bit guest address at a stride of 8 bytes, reloaded
from a module global at every site, with no bounds check and no miss path in the generated
code.** Confirmed on the second, third, fourth and fifth module. **MEASURED.**

### Geometry

| | kernel | Crimson | Blinx | Conker | Fuzion |
|---|---:|---:|---:|---:|---:|
| dispatch sites | 1,426 | 39,964 | 18,021 | 61,388 | 26,376 |
| … that reload the table base from a module global | **100 %** | **100 %** | **100 %** | **100 %** | **100 %** |
| shipped `(guest, host)` pair array | 786 KB | 20.4 MB | 12.0 MB | 43.5 MB | 18.5 MB |
| map density over the guest span | 78.30 % | 97.56 % | 97.73 % | 97.70 % | 97.83 % |
| distinct host targets / entries | 99.88 % | — | **99.96 %** | — | — |

The shipped array is `(guest_rva, host_rva)` as two 32-bit words, strictly increasing in the
guest column with no duplicates, and **not** monotonic in the host column (35–39 % of
consecutive entries step *backwards* in host address). **MEASURED.**

The runtime table the generated code actually jumps through is built from that array by the
host at init; the module publishes its base and count and nothing else. The index is the raw
guest address with no bias, shift or mask applied — the value comes straight off the guest
stack or out of a guest register — and the scale is 8. **MEASURED.**

Two consequences, both **INFERRED** but tightly constrained:

- Since the kernel module dispatches on guest addresses above 2 GiB and the game modules
  dispatch on guest addresses under 16 MiB, and a guest return can cross between them, there
  is **one global table spanning the guest address space**, not a per-module table. At 8 bytes
  per guest address that is a 32 GiB reservation, of which only the pages covering translated
  ranges need be committed.
- **A miss is not something the generated code can detect.** There is no compare, no range
  check, no alternative path: in a 6 MB window of the Blinx module, 7,669 dispatch sites, of
  which 129 (1.68 %) had any comparison within four preceding instructions and none of those
  were bounds checks. A miss therefore has to be handled by what the runtime put in the table
  slot, or by the slot's page not being committed. Which of the two, I could not determine.

### What is being dispatched

Classifying the 7,669 sites in a Blinx window by what feeds the index register:

| source of the guest target | share |
|---|---:|
| popped off the guest stack — a guest `ret` | **80.9 %** |
| a guest register — indirect call/jump through a register | 11.9 % |
| a guest memory load — vtable, import thunk, jump table | 7.3 % |

**MEASURED.** Guest *calls* are overwhelmingly resolved statically instead: of 54,154 guest
call sites in Crimson, 42,457 (78 %) push the real return address and then take a direct
relative jump into the callee's translation. The whole-module ratio of direct to indirect
control transfer is roughly 35:1. **MEASURED.**

### The arena is a mesh of tiny blocks, not "main stream plus recovery stubs"

This is the correction. The recompiler doc describes mid-instruction guest addresses as
resolving to "a recovery stub in a separate cold region, which fixes up state and jumps into
the optimized instruction stream". That is not what is there.

Sampling 3,000 map entries from the Blinx module at random and decoding from each:

- **99.96 % of entries have their own distinct host address.** Only 624 host targets in the
  whole 1.5 M-entry map are shared by more than one guest address. There is no common stub.
- Median distance from an entry point to its first terminator: **2–3 instructions, 19 host
  bytes**. Mean 42 bytes, with a long tail.
- 77 % terminate in a direct relative jump, and **99.3 % of those land on another map entry's
  host address**.
- 7.8 % terminate in a dispatch; 11 % run into a trap instruction.
- **4.4 % of all entry points begin with a trap instruction immediately** (3.6 % in the
  kernel module) — guest bytes whose decode is not a valid instruction are given a poisoned
  entry point rather than being left out of the map.

**MEASURED.** The picture is a uniform mesh: every guest byte has its own short translation of
whatever the instruction stream looks like starting at that byte, chained by direct jumps into
the translation of the next guest address it reaches, converging on the real instruction
stream within a couple of instructions because x86 self-synchronises. Entry points at real
instruction boundaries carry the long, optimized, fused blocks; interior bytes carry two-
instruction shadows. There is no cold region and no state fixup — the shadow code *is* a
correct translation of the misaligned decode.

That also independently confirms the correction already recorded in
[ms-fusion-corpus.md](ms-fusion-corpus.md#1-the-four-modules): map presence is not a coverage
oracle, because presence is universal.

---

## 5. Q4 — The two tiers

**The `_no` variant is the `Fb` (fallback) tier, and the entire difference at the build level
is one extra control file.** Confirmed with documentary evidence, on two independent module
pairs. **MEASURED.**

Each PowerPC module's version resource carries the compiler's own argument dump. Diffing the
`Pri` module's dump against its `_no` twin, for both `xboxkrnlcf.bin` and `xam.xex`, the
*only* differences are:

- one additional entry in the ordered control-file list, named for fallback, inserted into
  the middle of the list;
- the output/working directory and the per-module analysis-database path, which are suffixed
  `Pri` or `Fb` respectively;
- the compilation-info filename, likewise suffixed.

Every other argument — compiler path, the Lua script list, the trace-digest path, the
distributed-build parameters, the "update the analysis database" flag — is byte-identical.
**MEASURED.** The teardown's `Pri`/`Fb` reading is correct and now rests on two module pairs
rather than an inference from one filename.

Note that the analysis databases are *per tier*: the two tiers do not share discovered facts.
**MEASURED**, from the path suffixes.

### What the two tiers actually look like, on six pairs

| source | `Pri` entries | `Fb` entries | ×  | `Pri` .text | `Fb` .text | × |
|---|---:|---:|---:|---:|---:|---:|
| `xam.xex` | 93,424 | 1,044,969 | 11.2 | 10,274,132 | 24,720,340 | 2.41 |
| `xboxkrnlcf.bin` | 11,118 | 119,935 | 10.8 | 1,142,390 | 2,803,590 | 2.45 |
| `Xam.Community.xex` | 5,667 | 63,905 | 11.3 | 628,486 | 1,444,614 | 2.30 |
| `ximecore.xex` | 2,456 | 34,416 | 14.0 | 334,022 | 784,118 | 2.35 |
| `hud.xex` | 3,220 | 25,023 | 7.8 | 250,854 | 559,062 | 2.23 |
| `xefutitle.xex` | 26 | 722 | 27.8 | 15,126 | 27,558 | 1.82 |

**MEASURED.** `Pri`'s guest-address deltas cluster at 8/12/16/24 bytes (branch targets only,
one entry per 2–6 PowerPC instructions); `Fb`'s are 4 bytes almost exclusively (one entry per
PowerPC instruction). The `xboxkrnlcf` figures reproduce the teardown's 11,118 / 119,935
exactly, which validates the extraction on the other five.

**The file-size ratio the question asks about is 3–4× because `.text` grows 2.2–2.5× and the
address map grows 8–11×.** The map, not the code, is what makes the `_no` modules big.
**MEASURED.**

### Three codegen signatures that separate the tiers cleanly

| signature | `xam` `Pri` | `xam` `Fb` | `xboxkrnlcf` `Pri` | `xboxkrnlcf` `Fb` | `hud` `Pri` | `hud` `Fb` |
|---|---:|---:|---:|---:|---:|---:|
| degenerate zero-displacement jumps | 13 | **974,879** | 16 | **109,122** | 0 | **22,478** |
| byte-swapping load/store | **199,271** | **0** | **19,056** | **0** | **5,765** | **0** |
| separate byte-reverse instruction | 2,142 | 233,751 | 521 | 22,104 | 9 | 7,344 |
| 64-bit immediate loads | 1,424 | 80,525 | 162 | 9,967 | 16 | 2,299 |

**MEASURED.** Read them in order:

1. `Fb` emits one degenerate jump-to-the-next-instruction per guest instruction — 974,879 of
   them against 1,044,969 map entries, i.e. 93 % of guest instructions. Nothing is fused.
2. **`Fb` does not use the byte-swapping load/store form at all.** It loads, then reverses in
   a separate instruction. `Pri` folds the endian swap into every memory access for free.
   This is new: the teardown established that `Pri` uses the folded form, but not that the
   fallback tier abandons it. It is the single clearest illustration of what "unoptimized
   tier" means here — the same semantics, none of the instruction selection.
3. `Fb` never folds an immediate; `Pri` almost always does.

And structurally, in a 3 MB window of each: `Pri` makes 24,259 direct host calls against
1,019 indirect; `Fb` makes 449 direct against 12,499 indirect. **MEASURED.** Every call in the
fallback tier goes through the table.

**The x86 layer ships no fallback tier at all** — there is no `xefu_*_no.dll` in any of the
four packages. **MEASURED.** Its equivalent of `Fb` is the per-byte shadow mesh of §4, which
is inside the single optimized module.

---

## 6. Q5 — The helper surface

**One function pointer.** **MEASURED**, and this is smaller than the teardown estimated.

Every escape from translated x86 code is an indirect call through **a single slot** at offset
`+0x10` in the module's published pointer block, with a kind code in `r9` selecting the
service and the guest EIP in `r8`. In a 6 MB window of the Blinx module there were 28,636
such calls and **one distinct call target**. The dispatch table base is a second slot at
`+0x20`. The pointer block sits at the end of `.data` in four of the five modules, which bounds it at
13–19 slots; translated code references exactly those two, and `InitPrecompiledDll` writes a
timing ratio into a third at `+0x50`.
**MEASURED**, identical offsets in all four game modules and the kernel.

(The kernel module additionally reaches slots `+0x08`, `+0x18`, `+0x38` and `+0x40` from 136
sites in total; those are the only exceptions found anywhere in the corpus.)

### What the kinds cover — whole-module counts, byte-exact

| kind | kernel | Crimson | Blinx | Conker | Fuzion | what it is |
|---|---:|---:|---:|---:|---:|---|
| `0x1d` | 1,294 | 26,743 | 15,129 | 53,598 | 23,166 | preemption/interrupt poll |
| `0x1b` | 559 | 14,418 | 9,257 | 33,010 | 13,957 | segment load, step 1 |
| `0x1c` | 558 | 14,417 | 9,234 | 33,006 | 13,955 | segment load, step 2 |
| `0x45` | 925 | 15,493 | 9,684 | 29,975 | 13,942 | port read |
| `0x12` | 344 | 8,231 | 5,269 | 17,847 | 8,291 | segment commit (a) |
| `0x47` | 473 | 7,325 | 3,789 | 11,560 | 7,350 | port write |
| `0x1e` | 160 | 4,151 | 2,308 | 6,794 | 4,492 | second poll variant |
| `0x16` | 99 | 3,007 | 2,193 | 9,081 | 2,647 | segment commit (b) |
| `0x1f` | 269 | 3,480 | 2,977 | 5,187 | 3,062 | x87-adjacent |
| `0x26` | 232 | 5,073 | 1,306 | 5,234 | 2,854 | — |
| `0x13` | 108 | 2,789 | 1,632 | 5,439 | 2,681 | segment commit (c) |
| `0x25` | 69 | 2,403 | 914 | 3,573 | 2,215 | interpret one opcode |
| `0x43` | 72 | 1,057 | 1,999 | 2,756 | 1,294 | EIP-only callout |
| `0x14` | 3 | 230 | 86 | 491 | 201 | — |
| `0x15` | 4 | 160 | 54 | 148 | 135 | — |
| `0x4f` | 1 | 1 | 23 | 4 | 2 | — |
| `0x4b` | 11 | 0 | 0 | 0 | 0 | kernel only |
| `0xfe` | 1 | 1 | 1 | 1 | 1 | exactly one site per module |

**MEASURED.** Fourteen to sixteen kinds per module; the union across the corpus is eighteen.
The counts reproduce the teardown's Crimson figures to within rounding, which cross-validates
both methods.

New readings, **INFERRED** from argument fingerprints observed at multiple sites:

- **`0x1b`/`0x1c` are a segment-register load pair, not a floating-point control word.** The
  teardown listed `fldcw`/`fnstcw` or a far return as candidates. The observed shape is: read
  a 16-bit word from the guest stack into the helper's argument block, call `0x1b`, call
  `0x1c`, call exactly one of `0x12`/`0x13`/`0x16`, then advance the guest stack pointer by
  four. That is `pop <segment register>`: selector fetch, descriptor resolve, then a
  per-segment commit. It also explains why `0x1b` and `0x1c` counts are equal to within a
  handful in every module — they are always emitted as a pair — and why the counts are so
  large: the one-byte guest opcodes for segment pops are common in shadow decodes.
- **`0x25` really is a generic single-opcode interpreter**: the argument block receives a
  guest opcode byte and nothing else.
- **`0xfe` appears exactly once in every one of the five modules.** A singleton emitted per
  module rather than per site — a module-level abort or "unreachable" callout.

**What the helper surface does *not* cover:** ordinary memory access, unaligned access, and
arithmetic. There is no load helper, no store helper, no alignment helper, and no
software-FPU path — x87 and SSE guest instructions are translated to host x87 and SSE
directly — 20,554 scalar single-precision moves alone in a single 6 MB Conker window, and 668
x87 instructions across the whole 853 KB kernel module. **MEASURED.** The
escape hatch is device access, segmentation, preemption and one interpreter — not a long tail
of untranslatable instructions. The teardown's conclusion holds, now on five modules.

---

## 7. Q6 — Memory access

**A single base register plus a 32-bit guest address, with no check of any kind, and the
displacement form limited to the low 2 GiB.** **MEASURED.**

Two shapes, and only two:

1. `[guest-register + r15 (+ displacement)]` — used when the guest address is a guest register
   plus a small constant, or an absolute address that fits in a positive signed 32-bit
   displacement.
2. `[r15 + scratch-register]` — the guest address is first computed into a 32-bit scratch
   register (which zero-extends on write), then added to the base.

Because the displacement is sign-extended, shape 1 cannot reach guest addresses at or above
2 GiB. The corpus shows exactly that: across all five modules, **every** absolute
displacement measured lies in `0x0 … 0x7FFFFFFF` and **not one is negative**. And the module
whose guest addresses are all above 2 GiB — the recompiled kernel, which lives high in the
guest address space — uses the displacement form 191 times in its entire `.text`, against
22,787 times in a single 6 MB window of a game module, and correspondingly leans harder on
shape 2 (12.1 % of its memory operands against 9.0 %). **MEASURED.** The 32-bit scratch
register is the general mechanism precisely because zero-extension gives exact 4 GiB guest
wraparound for free.

**There is no bounds check, no address translation, no guard sequence and no software MMIO
test on any guest memory access in the corpus.** **MEASURED.**

**MMIO is therefore not distinguished in the generated code at all.** Port I/O has its own
escape kinds (`0x45`/`0x47`), but memory-mapped device space is addressed exactly like RAM.
**INFERRED**: the guest arena must be a 4 GiB reservation whose device pages are mapped
without access, so a guest access to them faults and the runtime's handler decodes and
services it. The generated code offers no other possibility — but the handler is in
`Emu.exe`, so this is inference from absence, not observation. It is also the standard
technique, and it is what this project already does for the NV2A and MCPX apertures.

Two smaller facts, both **MEASURED**:

- `r12` is touched from translated code at exactly **one** offset — a single byte, read
  before every preemption poll (1,134 / 22,592 / 12,821 / 46,804 / 18,674 sites). Nothing
  else in the per-thread block is visible to generated code.
- `r13` points at a **big-endian** state block. Its fields are read and written with the
  byte-reversing load/store form even in the x86 layer. The visible layout is six 12-byte
  slots beginning at `+0x20` — selector at slot start, 32-bit linear base four bytes in —
  which is the six x86 segment registers in architectural order, plus a 32-bit field at
  `+0xa4` holding the shadowed interrupt flag. A segment-overridden guest access costs one
  extra load of the segment base and nothing else; the thread-information-block idiom that
  SEH uses is a two-instruction sequence. **INFERRED** for the identification of the slots;
  the stride, the widths, the index-4 slot being the one used for thread-local access, and
  the big-endianness are all measured.

The big-endian state block in an x86-to-x86 translation is worth a sentence: the OG Xbox
emulator is itself a recompiled Xbox 360 title, so the shared CPU state block is laid out the
way the 360 layer wants it, and the x86 layer pays a byte-reverse to read it. **INFERRED.**

---

## 8. Q7 — Build drift

**The controlled experiment the question proposes does not exist in this package revision,
and the reason is itself the finding.** **MEASURED.**

`xefu_69c41281_00027bcf.dll` is **byte-identical across all four packages** (md5
`b8e42053e6305b77ec5c1e546deda766`), and so is every one of the fourteen PowerPC modules. More
to the point: **every shared module in all four packages carries build `20F912`**, including
in the package whose game module is `20F90F` and the one whose game module is `20F919`.

| | Crimson | Blinx | Conker | Fuzion |
|---|---|---|---|---|
| game module build | `20F90F` | `20F912` | `20F917` | `20F919` |
| shared kernel + all `xeo3_*` build | `20F912` | `20F912` | `20F912` | `20F912` |

So the shared modules are **not rebuilt per title**. They are compiled once, at `20F912`, and
copied into every package; only the per-title game module is compiled with whatever build was
current. **MEASURED.** There is consequently no same-source/different-build pair among the
shipped binaries in revision `2608.3123.1.0`, and Q7's controlled experiment cannot be run on
it.

This contradicts [ms-fusion-corpus.md §2](ms-fusion-corpus.md#2-build-drift--visible-only-across-the-corpus)
as originally written, which reported the kernel module differing in Conker. That document has
since been corrected in place against this same package revision, and my measurement agrees
with the correction independently: the original observation was true of `2607.x` and is not
true of `2608.3123.1.0`. Two readings are possible and I cannot choose between them: either
Microsoft rebuilt and re-unified the shared layer in the newer revision, or the earlier
package simply carried a stale copy in one title. **INFERRED, unresolved.**

It also confirms the corpus document's other correction: **Blinx's game module is `20F912`,
not `20F914`.** The `20F914` in the first document came from the emulator-layer module's path,
which is a different module. **MEASURED.**

### The drift test that can be run

With no same-source pair available, the remaining test is whether the four builds emit the
same codegen *vocabulary* for four different sources. They do, completely:

- identical dispatch idiom, identical pointer-block offsets (`+0x10` helper, `+0x20` table) in
  all four;
- identical escape convention and, bar two singletons, the identical kind-code set;
- identical high-byte workaround, preemption-poll sequence, `pushfd`/`popfd` sequences and
  segment-access sequences, **byte for byte in their encodings** — the byte-pattern scans in
  §3–§6 were written once and match all four builds without adjustment;
- identical memory-access shapes and the same 2 GiB displacement boundary;
- host-bytes-per-guest-byte within 2 % of each other (8.06 / 8.17 / 8.17 / 8.23).

**MEASURED.** Over four toolchain builds spanning the catalogue, **no observable codegen
difference**. Whatever changed between `20F90F` and `20F919` did not change the emitted
vocabulary. **INFERRED** — absence of difference in what I measured is not absence of
difference.

One incidental, and a refinement of the recompiler doc: the PowerPC modules' initialisation
does not merely "branch to a slower path" when hardware fused-multiply-add is missing. It
tests the CPUID bit and, when the feature is present, **patches four function pointers** to
the fast variants. A four-entry dispatch patch, not a branch. **MEASURED.**

---

## 9. Scoreboard against the existing documents

**Confirmed** (now on 2–5 modules instead of 1):

- Guest GPRs are host registers 1:1 on the x86 path; memory context block on the PowerPC path.
- Guest `esp` in `r14`, guest RAM base in `r15`, `r12`/`r13` as state blocks, `r8`–`r11`
  scratch.
- Flags are host flags on the x86 path; condition outcomes are materialised on the PowerPC
  path — and the 500–1,600× ratio in condition-set density quantifies it.
- Flag-preserving address arithmetic for every stack adjustment.
- Guest calls store the true guest return address and then jump.
- Dispatch is a flat, directly-indexed table with no hash, search, compare or miss path.
- `Pri`/`Fb` are primary and fallback tiers; `Fb` fuses nothing and routes every call
  indirectly; the x86 layer ships one tier.
- The escape surface is device access, segmentation, preemption and one opcode interpreter.
- Crimson's map is 2,546,633 entries — reproduced exactly from the same bounds.
- `xboxkrnlcf`'s tiers are 11,118 and 119,935 entries — reproduced exactly.

**Refined:**

- The helper surface is **one** call target from translated code, not ~12 slots. The pointer
  block has 13–19 slots; generated code uses two of them.
- `0x1b`/`0x1c` are a segment-load pair, resolved from "`fldcw` or far return".
- The `Fb` tier abandons the folded byte-swapping memory access entirely — a tier difference
  the earlier comparison did not reach.
- The FMA check patches four function pointers rather than branching.
- The `_no` size ratio is driven by the address map (8–11×), not the code (2.2–2.5×).
- The two tiers keep **separate** persisted analysis databases.

**Contradicted:**

- **"Mid-instruction addresses resolve to a recovery stub in a separate cold region which
  fixes up state."** There is no shared stub and no cold region: 99.96 % of map entries have
  their own distinct host address, and the median entry is a 2–3 instruction genuine
  translation of the misaligned decode that rejoins the stream by a direct jump (99.3 % of
  those jumps land on another map entry). 4.4 % of entry points are simply poisoned with a
  trap.
- **"Conker ships a distinct build of the recompiled kernel."** Not in this package revision;
  all shared modules are one build (`20F912`) and byte-identical everywhere.
- **Blinx's build is `20F912`,** not `20F914`.

---

## 10. What this changes for us

Ranked by value per unit of work, and split by what a C backend can and cannot have.

### A C backend can adopt these

1. **`tools/recomp/lifter.py`: shadow the interrupt flag, not the arithmetic flags.** We synthesise all
   flags because we must. But Microsoft's `pushfd`/`popfd` handling says something we can
   copy directly: the flags a guest *observes as a word* and the flags a guest *branches on*
   are different problems. `EFLAGS` materialisation only has to be correct at `pushfd`,
   `popfd`, and the flag-reading idioms — everywhere else only the individual predicate
   matters. If our lifter materialises a full `EFLAGS` word anywhere other than those points,
   that is pure cost. Cheap to check, cheap to fix.

2. **`tools/recomp/lifter.py`: dead-flag analysis is worth real money to us, not pennies.** Microsoft
   gets flags free and *still* substitutes flag-preserving forms where the flags are dead. We
   pay for every flag we compute, in C, with no hardware doing it as a side effect. Our
   `flag_state` threading already exists; the measurement to take is what fraction of guest
   arithmetic in JSRF has provably-dead flags, and the change is to emit nothing for those.
   This is the single highest-value item in this document for us, and it is the one where our
   position is *better* than Microsoft's, because eliding costs them nothing and saves us a
   lot.

3. **`tools/recomp`: our flat table is already the right shape — it is the *contents* that
   are sparse.** `translator.py` builds `g_flat_table` as one function pointer per guest byte
   over the code span, indexed by `va - base`, which is structurally what Microsoft has. The
   difference is entirely in what populates it: they put an entry at every guest byte, we put
   one at every detected function start. That is the gap, and basic-block granularity is the
   C-affordable step toward closing it. Two supporting facts from this corpus. First, the
   cost model: **a Microsoft title spends ~8 host bytes of code plus ~8 bytes of map per guest
   byte, ~16× the guest code size**, and they pay it on a 5.4 MB guest without comment — table
   density is not what should make us hesitate. Second, their index is the **raw guest VA with
   no bias or mask**, which is what lets one table serve returns that cross between the game
   module and the kernel module; if our lookup ever grows a second table, keep the index
   uniform across them.

4. **`tools/recomp`: poison, do not silently continue.** 4.4 % of Microsoft's entry points —
   66,732 of them in Blinx alone — begin with a trap instruction, because the guest bytes
   there do not decode. Our `*_stubs_unresolved.c` takes the opposite approach: it consumes
   the pushed return address so the stack stays balanced and execution continues. That is a
   defensible choice for keeping a run alive, and a bad one for finding out why it went wrong
   — the symptom surfaces a long way from the cause. An opt-in switch that makes an unresolved
   stub trap instead of returning would cost a few lines in `translator.py`, does not depend
   on item 3, and converts a class of late mystery faults into an immediate, addressed one.

5. **`tools/recomp/lifter.py`: segment access is two instructions, not a helper.** Their model keeps six
   12-byte segment slots in the CPU state block and a segmented access is "load the base,
   add". If our `fs:`-relative handling for SEH is doing anything more elaborate than reading
   a base out of a small struct and adding it, simplify it to that shape. See
   [seh-handling.md](seh-handling.md).

6. **`tools/recomp`: keep the escape surface to one entry point with a kind code.** They have
   exactly one call target from translated code and ~15 kinds. Our runtime callout surface
   should be audited against that: a wide callout surface is not a sign of thoroughness, it is
   a sign that things which should be translated are being escaped.

7. **`tools/fusion`: the tier-signature scans generalise.** The byte-template scanner written
   for §3–§6 identifies Fission idioms without decoding, at full-module scale, in seconds. It
   belongs next to `tools/fusion/lifter_diff.py` if any further comparison work is done.

### A C backend cannot adopt these, and should stop counting them as gaps

- **The 1:1 register mapping.** Already known. What is new is the *cost of the seam*: even
  Microsoft pays two extra instructions per high-byte access, and they pay it 100,438 times in
  one title. Our TLS-globals model has no seam; it has a uniform tax instead. Caching the live
  set into locals per function (already on the list, see
  [register-model.md](register-model.md)) remains the reachable part.
- **Free flags.** Confirmed decisively. No C-level trick recovers this.
- **Byte-dense entry points at *instruction* granularity.** We could afford the table; we
  cannot afford a C function per guest byte. Basic-block granularity is the C-compatible point
  on that curve, and this document does not change that recommendation — but note that the
  shadow mesh is how Microsoft avoids needing instruction-boundary certainty, and block
  granularity does *not* buy us that. We still need our boundaries to be right.
- **A single flat 32 GiB reservation indexed by guest VA.** We map guest memory at its
  original VA, which is better for memory access, but it means our dispatch table cannot use
  the same trick without its own large reservation. It has to be sized from the guest code
  span rather than the address space — which is what `translator.py` already does.
- **The `Fb` tier.** A second, precise, per-instruction translation of everything is a real
  answer to "we could not prove this statically", and at 2.4× code size it is an affordable
  one for Microsoft. For us it would mean a second full C translation of the title. Not
  reachable. Item 4 (poison) is the cheap approximation.

---

## 11. What I could not determine

- **What a dispatch miss actually does.** The generated code has no miss path. Whether the
  runtime fills unmapped slots with a poison pointer or leaves the table's pages uncommitted
  is inside `Emu.exe`, which is ACL-locked by store package protection and was not read.
- **The helper ABI's register-preservation contract.** Inferred from the absence of spills and
  reloads at 108,980 call sites; not confirmed, for the same reason.
- **Whether the guest arena is a 4 GiB reservation with device pages mapped no-access.** It is
  the only mechanism consistent with the complete absence of checks in generated code, but the
  mapping is done by the runtime.
- **The identity of kinds `0x26`, `0x14`, `0x15`, `0x1f`, `0x43`, `0x4b` and `0x4f`.** Their
  argument fingerprints are either empty or inconsistent across sites. `0x1f` is x87-adjacent
  and `0x43` carries only a guest EIP; beyond that I would be guessing.
- **Whether codegen differs between builds `20F90F` and `20F919` at all.** The shipped corpus
  has no same-source pair. Same-name/same-size XDK library functions across titles were tried
  as a substitute and rejected: the four titles link genuinely different XDK builds, so the
  guest bytes differ and the comparison is meaningless. Only the negative result — identical
  idiom vocabulary across four builds — is available.
- **Why the `2607.x` and `2608.x` revisions differ in whether the shared kernel module is
  unified.** Both readings (a rebuild-and-unify, or a stale copy in one package) fit.
- **Whether `Pri` and `Fb` are ever both resident for the same module at runtime**, and what
  selects between them. The build system produces both; nothing in the modules says how the
  runtime chooses.
- **The `0xfe` singleton's purpose.** One site per module, in all five modules, with no
  arguments.

---

## Appendix: reproducing this

`/usr/bin/python3` (capstone 5.0.7, pefile 2024.8.26, numpy 2.0.2). `tools/fusion/module.py`
parses the identity tables; everything below was done with short scripts over `pefile`'s
memory-mapped image.

- **Map bounds.** Disassemble `InitPrecompiledDll`, take its two `rip`-relative address
  computations that land in `.rdata`: they are the first and one-past-last entry of the
  `(guest_rva, host_rva)` pair array. The PowerPC modules put a header entry at the front and
  export base+8, so subtract one from the computed count. Do not scan for the array — a
  scan truncates at the first wide gap and under-reports (Conker by 60 %).
- **Entry-point structure.** Decode from each entry's host address; the terminator and its
  target are the interesting part, and testing whether the target is itself an entry gives the
  re-synchronisation rate. A random sample of a few thousand is plenty.
- **Poisoned entries.** Index the module image by the map's host column and test the first
  byte for the trap opcode.
- **Idiom counts.** Byte-template search over `.text`, not disassembly. The dispatch tail, the
  guest-call prologue, the escape sequence, the preemption poll and the `pushfd` splice each
  have a fixed encoding that is identical in all four builds, so one pattern set covers the
  corpus. This is exact and runs on the 45 MB module in seconds.
- **Instruction census.** Linear sweep with resynchronisation (on an invalid decode, advance
  one byte and continue). Coverage comes out at 99.9–100 % with ~1 resync per 3.5 KB. Remember
  §1's warning before reading any opcode frequency.
- **Tier comparison.** The compiler argument dump is in the `Comments` version-resource string
  of the PowerPC modules; the x86 modules in this revision carry an empty `Comments`. Diff the
  `Pri` and `_no` strings key by key.

Do not run a full decode of the 89 MB module and anything else at the same time.
