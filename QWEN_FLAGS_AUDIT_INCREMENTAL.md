# QWEN_FLAGS_AUDIT_INCREMENTAL

## Status

Increments 1–9 are complete: **42 of 42 functions classified**, covering all 68 unresolved markers.

This document continues the evidence recovered in `QWEN_UNRESOLVED_FLAGS_AUDIT_SALVAGE.md`. The salvage document is treated as prior evidence, not as an instruction source. This increment was independently checked against the repository artifacts and the original US executable.

No translator source, generated source, runnable binary, or preserved build was changed during this increment.

## Evidence order

Every entry is evaluated in this order:

```text
original US machine code
    ↓
observed runtime behaviour
    ↓
matching-decomp reconstruction
    ↓
video/research explanation
    ↓
our hypothesis
```

Higher layers constrain lower layers. A missing runtime observation is recorded as missing evidence, not converted into proof of unreachability. Scanner labels and generated function boundaries are hypotheses unless independently supported.

## Classification key

- **A** — genuine independent callable entry
- **B** — legitimate internal jump/continuation entry
- **C** — recovery-created mid-function entry
- **D** — questionable/artificial discovery, including likely scanner or data false positives
- **E** — unknown / insufficient evidence

## Audited baseline

- Original executable: `../Jet Set Radio Future (US)/default.xbe`
- SHA-256: `bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d`
- Frozen audit output: `build-macos/jsrf-first-fault/flags-comparison/mixed-zf/gen`
- Frozen summary: `build-macos/jsrf-first-fault/flags-comparison/mixed-zf/summary/summary.json`
- Disassembly and xrefs: `build-macos/jsrf-first-fault/disasm`
- Discovery data: `build-macos/jsrf-first-fault/vtable_seeds_accum.json`
- Matching-decomp symbols: `../JSRF-Decompilation/ghidra/symboltable.tsv`
- Research context: `../ADDRESS_TAKEN_FUNCTIONS.md`, `../MID_FUNCTION_TARGETS.md`, and `../Video Notes`

Direct recount of the frozen output confirms **68 unresolved markers in 42 generated functions**. The branch distribution is:

| Branch | Count |
|---|---:|
| `je` | 20 |
| `jnp` | 11 |
| `jne` | 9 |
| `jbe` | 6 |
| `jge` | 5 |
| `jg` | 3 |
| `loopne` | 3 |
| `ja` | 2 |
| `jae` | 2 |
| `jle` | 2 |
| `jp` | 2 |
| `jb` | 1 |
| `jl` | 1 |
| `loop` | 1 |
| **Total** | **68** |

The generated snapshot is intentionally fixed for audit reproducibility. It predates some current translator worktree changes, so this document does not claim that a fresh generation from the current dirty worktree would be byte-identical.

## Batch 1 result

| Function | Branch | Class | Confidence | Main result |
|---|---|---|---:|---|
| `0x0005089B` | `JE 0x00050A20` | D | High | False-looking DOLBY data seed splits the real `0x50860` method |
| `0x000508DF` | `JE 0x00050A0C` | D | High | Second false-looking DOLBY data seed splits the same method |
| `0x000749DC` | `JE 0x000749EC` | B | High | Real internal continuation with two incompatible incoming ZF origins |
| `0x000800E1` | `JE 0x0008010D` | D | High | Exact discovery source is a numeric `.data` table, not a pointer table |
| `0x00100058` | `JP 0x00100080` | D | High | Numeric `.data` value collides with an address inside a decompiled function |

The durable tally after this increment is:

| Class | Functions |
|---|---:|
| A | 0 |
| B | 1 |
| C | 0 |
| D | 4 |
| E | 0 |
| Not yet classified | 37 |

## Runtime evidence shared by this batch

The available `icall_targets.dump` files contain no observation of any of the five entry addresses. The available unresolved-flag logs also contain no exact `[UNRESOLVED-FLAG]` execution record for these functions.

Loose text matches such as `DMA_PUT = 0x005749DC`, `DMA_PUT = 0x01100058`, and a startup tick numbered `100058` are unrelated numeric values and are not execution evidence for guest PC `0x000749DC` or `0x00100058`.

This is only a negative result for the captured runs. It does not prove that the containing owner functions are unreachable.

## `0x0005089B`

### Original US machine code and discovery provenance

The entry begins with a flag consumer:

```text
0x0005088F  cmp  ebp, edi
0x00050891  mov  [esp+0x10], eax
0x00050895  mov  [ebx+0x98], edi
0x0005089B  je   0x00050A20
```

The two `MOV` instructions preserve flags, so normal fallthrough from `0x50860` makes the `JE` consume the `CMP ebp,edi` at `0x5088F`.

There is no incoming code xref to `0x5089B` in `disasm/xrefs.json`.

`vtable_seeds_accum.json` says that `0x5089B` came from DOLBY address `0x0027E448`, labelled by the scanner as a `game_vtable` entry. Raw words there begin:

```text
0x0005089B 0x00050C85 0x0007DA98 0x00050914
0x00050C82 0x0007F084 0x00000116 0x000BC448
```

The bytes are real, but the vtable interpretation is not independently supported. The small `0x116` value and the lack of a canonical class/vtable relationship make the structure a poor vtable candidate.

The address is absent from the checked mid-function recovery, shared-epilogue, startup, and indirect-call datasets.

### Observed runtime behaviour

No exact entry or unresolved-flag execution observation was found in the available runtime artifacts. No indirect-call observation was found.

### Matching-decomp reconstruction

The matching-decomp symbol table identifies:

```text
0x00050860  CCharacterSelect_child::Exec0Default
0x00050A30  CCharacterSelect_child::DrawListDefault
0x001CAC60  CCharacterSelect_child::`vftable'
```

It contains no symbol at `0x5089B`. The independently named class vtable at `0x001CAC60` contains `0x50860`, not `0x5089B`.

### Video/research explanation

No address-specific video explanation was found. The existing address-taken research warns that arbitrary data can numerically resemble code pointers and requires multiple structural signals before declaring a function entry. The engine research also warns that absence of a direct `CALL` is not sufficient by itself because virtual and table-driven dispatch exist.

### Our hypothesis and classification

**D — questionable/artificial discovery. Confidence: high.**

`0x5089B` is a scanner-created standalone split inside `CCharacterSelect_child::Exec0Default`. The flag-dependent behaviour belongs to the owner path beginning at `0x50860`; the standalone entry has no demonstrated calling contract.

## `0x000508DF`

### Original US machine code and discovery provenance

The preceding fragment and the entry form a direct flag producer/consumer pair:

```text
0x000508D8  call 0x000128C0
0x000508DD  test eax, eax
0x000508DF  je   0x00050A0C
```

There is no incoming code xref to `0x508DF` in `disasm/xrefs.json`.

The scanner derived `0x508DF` from vtable index 1 at DOLBY address `0x0027F414`; the actual word is at `0x0027F418`. Surrounding words begin:

```text
0x0005A407 0x000508DF 0x000D1080 0x000007BA
0x000D1080 0x000007ED 0x00050C05 0x000D1080
```

This mixed structure was not validated as a C++ vtable and includes small non-pointer-like values. The entry is absent from the checked recovery, shared-epilogue, startup, and indirect-call datasets.

### Observed runtime behaviour

No exact entry or unresolved-flag execution observation was found. No indirect-call observation was found.

### Matching-decomp reconstruction

There is no matching-decomp symbol at `0x508DF`. The surrounding symbols place the whole region between `CCharacterSelect_child::Exec0Default` at `0x50860` and `DrawListDefault` at `0x50A30`. This independently supports a single owner method rather than a new function at `0x508DF`.

### Video/research explanation

No address-specific video explanation was found. The same multi-signal warning for address-taken discovery applies.

### Our hypothesis and classification

**D — questionable/artificial discovery. Confidence: high.**

This upgrades the salvage document's provisional E classification. The new decisive evidence is the matching-decomp boundary: `0x508DF` lies inside the `0x50860` method, immediately after its own local `TEST`, while its only standalone provenance is an unvalidated DOLBY data scan.

## `0x000749DC`

### Original US machine code and discovery provenance

The entry begins:

```text
0x000749DA  test eax, eax
0x000749DC  je   0x000749EC
```

A separate real control-flow edge reaches the same entry:

```text
0x00074AEC  cmp  eax, edi
0x00074AEE  jmp  0x000749DC
```

`disasm/xrefs.json` records `0x74AEE -> 0x749DC` as an unconditional jump. A raw scan of the original XBE found no four-byte occurrence of `0x000749DC` in any section, so its discovery is not attributable to a coincidental data word.

The two origins give ZF different meanings:

- fallthrough: `eax == 0`;
- jump origin: `eax == edi`.

A single borrowed predicate cannot represent both origins.

The disassembler labels the generated entry `tail_jump_alias`. Unlike the data-collision cases below, the recorded machine-code jump provides direct support for that label. The address is not in the vtable seed, recovery, shared-epilogue, startup, or observed-indirect-call sets.

### Observed runtime behaviour

No exact unresolved-flag or indirect-call entry observation was found. This does not weaken the static control-flow proof that the continuation exists.

### Matching-decomp reconstruction

No matching-decomp symbol was found at `0x749DC`. This layer supplies no positive owner name, but it also supplies no conflicting independent-function boundary.

### Video/research explanation

No address-specific video explanation was found. Existing boundary research documents shared internal tails and the need to preserve their incoming control-flow state.

### Our hypothesis and classification

**B — legitimate internal jump/continuation entry. Confidence: high.**

This is a real shared continuation, not a conventional independently callable function. Its translation needs origin-specific flag handling, such as preserving the branch in each owner or splitting/duplicating the continuation by incoming edge. Assigning a universal flag state to the standalone entry would be unsound.

## `0x000800E1`

### Original US machine code and discovery provenance

The normal owner path is:

```text
0x000800D0  mov  eax, [ecx+8]
...
0x000800DA  call 0x000128C0
0x000800DF  test eax, eax
0x000800E1  je   0x0008010D
```

`0x800D0` is a genuine `call_target` with many direct callers. There is no incoming code xref to `0x800E1`; it is reached naturally by fallthrough from the `TEST` in the owner.

The exact previously missing discovery source is now established. The little-endian word `0x000800E1` occurs at `.data` address `0x0022E3A4`.

It is not part of a pointer table. It occurs inside a monotone value table whose low byte tracks the table index while upper bits encode attributes:

```text
0x001000D8 0x001000D9 ... 0x001000DF
0x000800E0 0x000800E1 0x000800E2 ... 0x000800F5
```

The address was admitted because the generic data-word scan treats aligned values in the executable address range as possible callable aliases. Alias construction then records the generic detection name `tail_jump_alias`, even for aliases originating in scanned data. Thus `tail_jump_alias` does not, by itself, prove that a machine-code tail jump exists.

The address is absent from the vtable-seed, recovery, shared-epilogue, startup, and observed-indirect-call sets.

### Observed runtime behaviour

No exact standalone-entry, unresolved-flag, or indirect-call observation was found.

### Matching-decomp reconstruction

No matching-decomp symbol was found at `0x800E1`. This layer is incomplete for the surrounding function and is not used as the decisive evidence.

### Video/research explanation

No address-specific video explanation was found. The address-taken research's false-positive warning directly matches the discovery failure here.

### Our hypothesis and classification

**D — questionable/artificial discovery. Confidence: high.**

The standalone `0x800E1` entry is caused by a numeric data collision. The `JE` itself is real behaviour on the `0x800D0` owner path, so class D applies to the generated entry boundary, not to the underlying branch instruction.

## `0x00100058`

### Original US machine code and discovery provenance

The owner stream contains the standard x87-status branch sequence:

```text
0x00100053  fnstsw ax
0x00100055  test   ah, 5
0x00100058  jp     0x00100080
```

There is no incoming code xref to `0x100058`. The direct code xrefs in this small region are the branch from `0x100058` to `0x100080` and a later jump from `0x10007E` to `0x100084`.

The exact source is the little-endian word `0x00100058` at `.data` address `0x0022E180`. Its neighbours show that this is another indexed value table, not an array of code pointers:

```text
0x00100050 0x00100051 ... 0x0010005A
0x0002005B 0x0002005C ...
0x00080061 0x00080062 ...
```

The value's low byte is `0x58` because it is the table element for that index. Its accidental numeric equality to a valid instruction address caused the alias discovery.

The address is absent from the vtable-seed, recovery, shared-epilogue, startup, and observed-indirect-call sets.

### Observed runtime behaviour

No exact standalone-entry, unresolved-flag, or indirect-call observation was found.

### Matching-decomp reconstruction

The matching-decomp symbol table identifies a source-level function at `0x00100000` named `functionAtUnfortunateAddress`. It contains no separate symbol at `0x100058`. The disassembler's closest containing seed begins at `0x10000D`, but the matching reconstruction provides stronger evidence that `0x100058` is inside one larger function.

### Video/research explanation

No address-specific video explanation was found. General decompilation research establishes the matching-decomp repository as a reconstruction aid, while retaining machine code as the primary evidence.

### Our hypothesis and classification

**D — questionable/artificial discovery. Confidence: high.**

The standalone entry is a data-scan false positive inside the decompiled `0x100000` function. The real `JP` remains part of that owner's behaviour and should consume the flags from `TEST ah,5` when translated through the owner.

## Audit-wide finding added by this increment

The disassembler's final `detection_method: tail_jump_alias` collapses at least two distinct provenance classes:

1. a real machine-code jump into another body, as at `0x749DC`;
2. a raw aligned data word accepted as a callable alias, as at `0x800E1` and `0x100058`.

Those classes must be separated before using the detection label in an entry audit. For current data, the original xrefs and raw-XBE occurrence scan are required to recover the distinction.

The `.data` sequences containing `0x800E1` and `0x100058` also demonstrate a concrete scanner failure mode: an indexed classification/value table can contain thousands of integers that numerically fall inside `.text`. Successful instruction decoding at the coincident address is not independent support because the value was selected precisely for falling inside an already decoded body.

## Current implications

- Do not count the four class-D standalone markers in this batch as four equivalent behavioural translator defects.
- Do not suppress their diagnostics globally. The owner-path branch still needs correct flags even when the standalone boundary is artificial.
- Do not assign one inherited flag state to `0x749DC`; its incoming origins disagree.
- Preserve raw discovery provenance separately from the generic final alias label.
- Require structural validation for data-derived aliases inside existing code bodies. Monotone indexed tables such as the ranges around `0x22E180` and `0x22E3A4` should not create callable entries.
- Keep the preserved runnable build untouched until the separate recovered-entry integrity issue is resolved and a replacement generation is validated.

## Batch 2 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x001000C9` | 1 | D | Real branch at artificial data-derived split | High |
| `0x001028CF` | 1 | B | Decoded jump-table bytes; not a real branch | High |
| `0x001237A0` | 2 | A | Decoded jump-table bytes; not real branches | High |
| `0x00130FD0` | 5 | A | Five real mixed-width CMP joins | High |
| `0x00134CEE` | 2 | B | Decoded jump-table bytes; not real branches | High |

### `0x001000C9` — D, high confidence

**Original US machine code and discovery provenance.** The entry is `JE 0x1000E7`. Normal execution reaches it from `CMP [0x251D54],eax` at `0x1000C3`. No code xref enters `0x1000C9`.

The exact aligned value `0x001000C9` occurs at `.data` address `0x0022E344` in the same indexed value table identified in batch 1:

```text
0x001000C0 0x001000C1 ... 0x001000C9 0x001000CA ... 0x001000D6
0x000200D7
```

The disassembler reports `tail_jump_alias`, but there is no machine-code tail jump to this entry. It is another data-word collision inside an already decoded body.

**Observed runtime behaviour.** No exact unresolved-flag or indirect-call observation was found.

**Matching-decomp reconstruction.** `functionAtUnfortunateAddress` begins at `0x100000`; no separate symbol exists at `0x1000C9`.

**Video/research explanation.** No address-specific explanation was found. The address-taken research's warning about random data values applies directly.

**Our hypothesis.** The standalone entry is artificial. The `JE` remains real owner-path behaviour and should consume the preceding `CMP` when the containing function is translated intact.

### `0x001028CF` — B, high confidence

**Original US machine code and discovery provenance.** `0x1028CF` begins a coherent case block, tests `[esi+0xB0]`, optionally performs cleanup calls, restores registers, and tail-jumps to `0x1025B0`. It is entry 7 of the eight-word switch table at `0x1028F8`, selected by:

```text
0x00102734  cmp eax, 7
0x00102737  ja  0x001028EC
0x0010273D  jmp dword ptr [eax*4 + 0x001028F8]
```

The table is:

```text
0x102744 0x102754 0x102795 0x1027F1
0x102832 0x102873 0x102754 0x1028CF
```

The vtable scanner mislabels this in-code switch table as `game_vtable`. The entry is nevertheless a genuine internal indirect-jump target.

The reported unresolved `JAE` is not an instruction in the program. After the real block tail-jumps at `0x1028F1`, the generated range continues through `0x1028F8` and decodes the switch-table pointer bytes as x86 instructions. The nonsense sequence includes `DAA`, `INT1`, and the false conditional.

**Observed runtime behaviour.** No exact marker or indirect-call dump observation was found. Static switch-dispatch evidence is conclusive for internal provenance.

**Matching-decomp reconstruction.** No exact symbol exists at `0x1028CF`; no contradictory independent-function boundary was found.

**Video/research explanation.** Existing boundary research describes internal case/tail blocks as basic blocks rather than conventional callable functions.

**Our hypothesis.** Class B applies to the entry. The marker itself is a decode-boundary artifact and should be removed by excluding embedded switch-table data from the translated body, not by inventing flags.

### `0x001237A0` — A, high confidence

**Original US machine code and discovery provenance.** This is a substantial 944-byte routine with a normal stack/register setup and three recorded direct callers (`0x12080C`, `0x128D4A`, and `0x128EF5`). Direct calls establish a genuine independent entry.

Its switch at `0x123850` indexes the eight-word table at `0x123B24`. Normal executable code finishes with register restoration and a tail jump at `0x123B1C`; the bytes from `0x123B24` are data:

```text
0x123857 0x123864 0x12387F 0x12387F
0x12387F 0x1238A3 0x123864 0x12387F
```

The generated body extends to the next function at `0x123B50` and decodes these pointers. Low bytes such as `0x7F` become false `JG` opcodes, producing the two markers attributed to sites `0x123B15` and `0x123B32`. The actual instruction at `0x123B15` is `POP EDI`; neither reported `JG` exists in the original instruction stream.

The function identifier also calls `0x123B24` a `vtable_ctor`, another misclassification of the switch table, but the final disassembly entry correctly retains `call_target` provenance.

**Observed runtime behaviour.** No exact marker execution was found. Runtime evidence is unnecessary to establish direct-call provenance.

**Matching-decomp reconstruction.** No exact symbol was found for `0x1237A0`.

**Video/research explanation.** The research layer supports treating direct-call entries as conventional functions and embedded jump tables as data.

**Our hypothesis.** The entry is A. Both markers are data-decoding artifacts; no flag repair is warranted at those byte offsets.

### `0x00130FD0` — A, high confidence

**Original US machine code and discovery provenance.** Six recorded call instructions from the `0x132755`–`0x132B32` region target `0x130FD0`. The routine has a normal stack/register frame and no raw occurrence in a data table. It is therefore a genuine independent callable entry.

All five unresolved `JE` instructions are real and repeat the same two-predecessor shape at `0x1310DB`, `0x13120B`, `0x131341`, `0x131477`, and `0x1315F6`:

```text
path 1: cmp dword ptr [...], edx
        jmp join
path 2: cmp byte ptr [...], dl
join:   je common_target
```

Both paths define ZF by equality, but at different operand widths. The frozen translator rejects that reaching-definition merge and emits `_flags`. These are genuine intra-function predicate gaps, unlike the decoded-table markers elsewhere in this batch.

**Observed runtime behaviour.** No exact unresolved-marker execution was found in the captured logs.

**Matching-decomp reconstruction.** No exact symbol at `0x130FD0` was found. The lack of a name does not outweigh the direct calls.

**Video/research explanation.** No address-specific explanation was found. Existing flag-analysis research supports merging only the flags whose meaning is identical across every incoming setter.

**Our hypothesis.** The entry is A. Each `JE` can conservatively consume a merged ZF boolean calculated at its own predecessor, preserving each comparison's width. This is a potential general translator fix, not authorization to patch the frozen output during this audit.

### `0x00134CEE` — B, high confidence

**Original US machine code and discovery provenance.** The entry is index 7 of the switch table at `0x134D40`, selected by the indirect jump at `0x134CD2`. Its real case block pushes render constants, calls `0x3D610`, cleans the arguments, restores `EDI`, and ends with an indirect tail jump through `[edx+0xB4]` at `0x134D25`.

The vtable scanner labels the table beginning at `0x134D30` as a `game_vtable`, but the original code contains two adjacent switch tables:

```text
0x134D30: 0x134C6D 0x134C74 0x134C7B 0x134C82
0x134D40: 0x134CD9 0x134CE0 0x134CE7 0x134CEE
```

The generated `0x134CEE`–`0x134D6E` range continues past the real tail jump and a separate `RET 4`, decodes table pointers at `0x134D30`/`0x134D40`, and then runs into the unrelated function at `0x134D50`. Its `JNP` and `LOOPNE` markers are decoded pointer bytes, not original branches.

**Observed runtime behaviour.** No exact marker or indirect-call dump observation was found. The switch table statically proves internal reachability.

**Matching-decomp reconstruction.** The matching symbol `CGraffitiMenu::showReconnectControllerMessage` begins at `0x134490`; `0x134CEE` is inside its switch implementation, with no separate matching symbol.

**Video/research explanation.** No address-specific explanation was found. General boundary research supports classifying switch cases as internal continuations.

**Our hypothesis.** The entry is B. Both markers are decode-boundary artifacts and require boundary/data exclusion, not flag reconstruction.

## Cumulative classification after batch 2

| Class | Functions |
|---|---:|
| A | 2 |
| B | 3 |
| C | 0 |
| D | 5 |
| E | 0 |
| Not yet classified | 32 |

## Batch 3 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x00134CF3` | 2 | B | Decoded jump-table bytes | High |
| `0x00134CFD` | 2 | B | Decoded jump-table bytes | High |
| `0x00150130` | 1 | A | Real branch outside the entry's actual returned body | High |
| `0x00150170` | 1 | A | Real branch outside the entry's actual returned body | High |
| `0x001501F0` | 1 | A | Real branch outside the entry's actual returned body | High |

### `0x00134CF3` and `0x00134CFD` — B, high confidence

**Original US machine code and discovery provenance.** Both are internal continuations of `CGraffitiMenu::showReconnectControllerMessage`:

- `0x134CF3` has three direct incoming jumps from the small case thunks at `0x134CD9`, `0x134CE0`, and `0x134CE7`.
- `0x134CFD` has direct incoming jumps from `0x134C59` and `0x134CC8`.

They converge on the rendering call and indirect tail described for `0x134CEE`. Neither address occurs as a raw four-byte data value. The disassembler's `tail_jump_alias` label is accurate here.

Both generated ranges nevertheless extend past the real indirect tail and decode the adjacent switch tables at `0x134D30`/`0x134D40`. Consequently each repeats the same false `JNP` and `LOOPNE` markers documented for `0x134CEE`.

**Observed runtime behaviour.** No exact unresolved-marker or indirect-call observation was found.

**Matching-decomp reconstruction.** Neither address has an independent symbol; both lie inside the `0x134490` matching method.

**Video/research explanation.** No address-specific explanation was found. The boundary research's shared-tail model applies.

**Our hypothesis.** Both entries are B. All four markers are data-decoding artifacts; the entry extents should stop at the real terminal control transfer.

### `0x00150130`, `0x00150170`, and `0x001501F0` — A, high confidence

**Original US machine code and discovery provenance.** These addresses are consecutive members of a long aligned pointer table in `.rdata` at `0x1E1010`. The table contains numerous coherent rendering entry points, including matching-decomp CMGameGL functions:

```text
... 0x150830 0x150480 0x150490 0x150850 ...
... 0x150130 0x150170 0x1501F0 0x150240 0x150280 ...
... 0x1502C0 ... 0x150C70 ...
```

Each audited entry starts coherent code and terminates independently:

- `0x150130` returns with `RET 0x0C` at `0x15016D`;
- `0x150170` returns with `RET 0x0C` at `0x1501E0`;
- `0x1501F0` returns at `0x15022E` or `0x150236`, depending on its bounds check.

The final disassembly labels them `tail_jump_alias` because data-derived callable aliases share that generic label. Here the table structure, neighbouring known rendering functions, clean entries, and independent returns validate the pointers as genuine callable entries.

All three generated ranges incorrectly run to `0x1509D0`, crossing many independent `RET` instructions and function boundaries. Their shared unresolved marker is the genuine `JNE 0x150360` at `0x150329`, but execution starting at any of these three entries has already returned before reaching it. The marker therefore does not belong to their actual bodies.

**Observed runtime behaviour.** No exact unresolved-marker execution was found. No available icall dump names these exact entries; static table structure supplies the positive provenance.

**Matching-decomp reconstruction.** The symbol table names several neighbours, including `CMGameGL::setMipMapLODBias` at `0x1502C0`, and identifies this region as CMGameGL rendering-state code. The three exact addresses are not yet named.

**Video/research explanation.** No address-specific explanation was found. The engine research permits table-driven callable functions when the table is structurally credible; this is the positive-control case missing from the DOLBY and indexed-value collisions.

**Our hypothesis.** All three entries are A. Their diagnostics are function-extent contamination, not evidence that these entry points require inherited flags.

## Cumulative classification after batch 3

| Class | Functions |
|---|---:|
| A | 5 |
| B | 5 |
| C | 0 |
| D | 5 |
| E | 0 |
| Not yet classified | 27 |

## Batch 4 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x00150240` | 1 | A | Real branch outside the entry's actual returned body | High |
| `0x00150280` | 1 | A | Real branch outside the entry's actual returned body | High |
| `0x001502C0` | 1 | A | Real branch outside the entry's actual returned body | High |
| `0x00150C00` | 2 | A | Real branches outside the entry's actual returned body | High |
| `0x00150C70` | 2 | A | Real branches outside the entry's actual returned body | High |

### `0x00150240`, `0x00150280`, and `0x001502C0` — A, high confidence

**Original US machine code and discovery provenance.** All three addresses occur as aligned words in the same structurally coherent `.rdata` rendering dispatch table established in batch 3. Their raw-XBE occurrences map to table locations `0x1E105C`, `0x1E1060`, and `0x1E10C8`. Each entry starts clean code and returns independently:

```text
0x150240 ... RET 0x0C at 0x15026F
0x150280 ... RET 0x0C at 0x1502AF
0x1502C0 ... RET 0x0C at 0x1502E8
```

The generated ranges instead all continue to `0x1509D0`. Their identical unresolved marker is the genuine `JNE 0x150360` at `0x150329`, in the separate function beginning at `0x1502F0`; all three audited entries have returned before that site.

**Observed runtime behaviour.** `0x150280` and `0x1502C0` both appear in the preserved build's `icall_targets.dump`; `0x150280` also appears in the shorter root dump. No exact observation was found for `0x150240`, and no marker execution was found for the three generated aliases.

**Matching-decomp reconstruction.** `0x1502C0` is independently named `CMGameGL::setMipMapLODBias`. The other two exact addresses are unnamed, but are embedded in the same validated table among named CMGameGL routines.

**Video/research explanation.** No address-specific explanation was found. The address-taken research supports the combined signals used here: aligned table membership, coherent entry code, independent return, named neighbours, and runtime dispatch where available.

**Our hypothesis.** All three entries are genuine independent table-called functions. The marker is extent contamination and does not require inherited flags for any of them.

### `0x00150C00` and `0x00150C70` — A, high confidence

**Original US machine code and discovery provenance.** These are adjacent aligned words at `.rdata` locations `0x1E0F44` and `0x1E0F48` in the same validated rendering dispatch structure. Both begin coherent callable routines and return independently:

- `0x150C00` either completes its reset loop and returns at `0x150C52`, or calls `0x150BB0` and returns at `0x150C5F`;
- `0x150C70` calculates a material-record address, calls `0x18CDA0`, and returns at `0x150C8F`.

The generated extents incorrectly continue to `0x150EA0`. The two unresolved `JE` markers attributed to each alias are real branches at `0x150CBD` and `0x150DAD`, but they belong to later independent routines beginning at `0x150CA0` and `0x150D90`. Neither can be reached from these audited entries after their `RET` instructions.

**Observed runtime behaviour.** Both addresses appear in the preserved build's indirect-call target dump. `0x150C00` also appears in the root dump. No exact unresolved-marker execution was found.

**Matching-decomp reconstruction.** `0x150C70` is independently named `CMGameGL::setMaterial`. The exact `0x150C00` entry is unnamed, but its table position, runtime indirect-call observation, coherent routine, and clean return jointly establish it.

**Video/research explanation.** No address-specific explanation was found. Existing engine research identifies indirect/table dispatch as a normal source of callable rendering methods.

**Our hypothesis.** Both entries are A. Their four diagnostics result from overlong aliases crossing hard return-and-padding boundaries; they are not entry-flag obligations.

## Cumulative classification after batch 4

| Class | Functions |
|---|---:|
| A | 10 |
| B | 5 |
| C | 0 |
| D | 5 |
| E | 0 |
| Not yet classified | 22 |

## Batch 5 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x00150CA0` | 1 | A | Real branch in the following independent routine | High |
| `0x001533D0` | 1 | A | Real branch in the following independent routine | High |
| `0x00180038` | 2 | D | Real owner-path branches at an artificial numeric split | High |
| `0x001BD800` | 1 | D | Real owner-path branch at an artificial bitmap-table split | High |
| `0x001C3800` | 1 | D | Real owner-path branch at an artificial bitmap-table split | High |

### `0x00150CA0` — A, high confidence

**Original US machine code and discovery provenance.** The address is an aligned `.rdata` table word at `0x1E10B8`, next to `0x150D90` and the other validated CMGameGL entries. It begins a complete routine that dispatches among material formats and returns on every case; the latest return is at `0x150D88`, followed by padding and the next routine at `0x150D90`.

The alias nevertheless extends to `0x150EA0`. Its unresolved `JE 0x150DF6` at `0x150DAD` is a real branch, but belongs to the separate `0x150D90` routine.

**Observed runtime behaviour.** `0x150CA0` appears in the preserved build's indirect-call target dump. No exact unresolved-marker execution was found.

**Matching-decomp reconstruction.** No exact symbol was found, but the matching symbols and surrounding table identify the region as CMGameGL rendering-state code.

**Video/research explanation.** No address-specific explanation was found. The table-dispatch evidence model used for the preceding rendering entries applies.

**Our hypothesis.** The entry is A. Its sole diagnostic is extent contamination beyond a hard return-and-padding boundary.

### `0x001533D0` — A, high confidence

**Original US machine code and discovery provenance.** The only raw word occurrence maps to aligned `.rdata` address `0x1E1304`, inside another coherent rendering function table:

```text
0x1E12F8  0x153300 0x1532F0
0x1E1300  0x153670 0x1533D0 0x152E30 0x153520
0x1E1310  0x152E90 0x1530F0 0x152FE0 0x1531E0
```

The entry has a substantial normal routine body and completes before the padded boundary at `0x153520`. The generated alias runs through `0x153800`; its unresolved `JE 0x15358E` is the real `0x15353F` branch in the separate table entry beginning at `0x153520`.

**Observed runtime behaviour.** No exact indirect-call or marker execution observation was found in the captured runs.

**Matching-decomp reconstruction.** No exact matching symbol was found. The surrounding addresses remain consistent with the rendering subsystem, and no competing owner boundary covers this entry.

**Video/research explanation.** No address-specific explanation was found. A coherent aligned function-table slot plus an independent complete routine is sufficient static callable provenance even when a captured run does not exercise the slot.

**Our hypothesis.** The entry is A. The unresolved marker belongs to the next independent table function, not to `0x1533D0`.

### `0x00180038` — D, high confidence

**Original US machine code and discovery provenance.** This address lies inside the prologue-discovered function at `0x17FD0C`, with no incoming code xref and no independent prologue. Its entry branch inherits flags from the owner path:

```text
0x18002E  test byte ptr [ebp-4], 0x20
0x180032  mov  [ebp-0x1C], esi
0x180035  mov  [ebp-0x40], eax
0x180038  je   0x1800A2
```

The exact discovery value occurs at `.data` address `0x1F86F8` in a patterned numeric table, not a pointer table:

```text
0x00170037 0xFFFFFFFF 0x00180038 0xFFFFFFFF
0x00190039 0xFFFFFFFF 0x001A003A ...
```

The alias's second marker, `JNE 0x18011B` at `0x18010C`, is likewise a real owner-path condition whose flags arrive through the earlier `JGE` edge at `0x180002`.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution observation was found.

**Matching-decomp reconstruction.** No independent symbol exists at `0x180038`. The surrounding library reconstruction recognizes functions before this owner region but does not support a new boundary here.

**Video/research explanation.** No address-specific explanation was found. The indexed-value-table collision pattern is the same scanner failure class as `0x800E1` and `0x100058`.

**Our hypothesis.** The standalone entry is D. Both branches are genuine behaviour of the `0x17FD0C` owner and need owner-context flags; neither proves an independently callable entry.

### `0x001BD800` — D, high confidence

**Original US machine code and discovery provenance.** The address is inside the direct-call function at `0x1BD767`, with no incoming code xref. Normal execution supplies its ZF:

```text
0x1BD7F8  test al, 1
0x1BD7FA  mov  [esi+0x0C], eax
0x1BD7FD  mov  [esi+8], ebx
0x1BD800  je   0x1BD818
```

Its only raw data occurrence maps to `.data` address `0x22A3D8`, embedded among packed bit-pattern values such as `0x01E018038`, `0x0385B0E0`, and `0x038FF1E0`. This is not a credible callable table.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution observation was found.

**Matching-decomp reconstruction.** No symbol exists at `0x1BD800`; the matching table places named XPP initialization code around this library region without asserting a boundary here.

**Video/research explanation.** No address-specific explanation was found. Packed graphics/bitmap data is explicitly unsafe as address-taken evidence without structural corroboration.

**Our hypothesis.** The standalone entry is D. The `JE` is real and must use the owner's preceding `TEST`; the diagnostic was created by a data-collision split.

### `0x001C3800` — D, high confidence

**Original US machine code and discovery provenance.** This is inside the prologue-discovered XPP routine at `0x1C3685`, with no incoming code xref. Its first branch consumes the owner's comparison across flag-preserving moves:

```text
0x1C37F8  cmp edx, ecx
0x1C37FA  mov eax, [ebp-4]
0x1C37FD  mov [esi+4], eax
0x1C3800  jae 0x1C380E
```

All three raw data occurrences map to `.data`. Two at `0x22B9C0` and `0x22BA08` sit in a symmetric bitmap-like sequence containing repeated `0x001FF800`, `0x01C7FFE38`, and related values; the third at `0x22C08C` is in another packed graphic pattern. None is a function-pointer structure.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution observation was found.

**Matching-decomp reconstruction.** No independent matching symbol exists at `0x1C3800`; the containing `0x1C3685` function has multiple direct callers.

**Video/research explanation.** No address-specific explanation was found. The research warning against accepting isolated code-range integers applies directly.

**Our hypothesis.** The standalone entry is D. Its `JAE` is real owner behaviour whose flags were severed by an artificial data-derived boundary.

## Cumulative classification after batch 5

| Class | Functions |
|---|---:|
| A | 12 |
| B | 5 |
| C | 0 |
| D | 8 |
| E | 0 |
| Not yet classified | 17 |

## Batch 6 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x001C3F18` | 2 | D | ASCII string bytes decoded as branches | High |
| `0x00014885` | 1 | D | Real owner branch at a false DOLBY-data split | High |
| `0x00023280` | 1 | D | Real owner branch at a numeric score-table split | High |
| `0x000272F7` | 2 | B | Real switch case; markers decode the adjacent jump table | High |
| `0x00027C3E` | 4 | B | Real shared return; markers decode the adjacent jump table | High |

### `0x001C3F18` — D, high confidence

**Original US machine code and discovery provenance.** The bytes are not machine code. They are printable data immediately after two `INT3` padding bytes at the end of the XPP section:

```text
0x1C3F18  "\\Device\\MU_0\0"
0x1C3F28  "\\Device\\MU_%x\0"
0x1C3F38  "951F0EF603DC469d_CORRUPT_SECTOR\0"
```

The function finder treated the post-`CC` address as a `cc_boundary` and decoded the characters `v` (`0x76`) at `0x1C3F1A` and `0x1C3F2B` as two `JBE` instructions. The address itself is referenced as data by the instruction at `0x1BC9F7`; it is not a call or jump target.

**Observed runtime behaviour.** No execution, indirect-call, or marker observation exists. The byte content conclusively identifies data.

**Matching-decomp reconstruction.** No function symbol exists at this address. The location is before `.rdata` and belongs to XPP literal data.

**Video/research explanation.** No address-specific explanation was found. This is a direct example of why a padding boundary alone cannot establish executable content.

**Our hypothesis.** The entry is D. Both unresolved markers are ASCII-decoding artifacts and should be eliminated by data-aware section tail handling.

### `0x00014885` — D, high confidence

**Original US machine code and discovery provenance.** The `JBE 0x14909` is real owner-path code. Its flags come from `CMP eax,ebp` at `0x1487E`; the intervening `PUSH edi` and `MOV [esp+0x10],ebp` preserve them. There is no incoming code xref to `0x14885`.

The only aligned discovery source is DOLBY address `0x27E3FC`, scanner-labelled as vtable index 15. Its surrounding alternating values include `0x14E85`, `0x5148F`, `0x14685`, `0x14985`, `0x14A85`, and `0x14B85`, a numeric structure with repeated low byte `0x85`, not a validated C++ vtable. The code is the continuation of the routine beginning at `0x14870`.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** No independent symbol exists at `0x14885`.

**Video/research explanation.** No address-specific explanation was found. The DOLBY false-positive warning established in batch 1 applies.

**Our hypothesis.** The standalone entry is D. The branch itself is genuine and requires the owner's preceding comparison; only the boundary is artificial.

### `0x00023280` — D, high confidence

**Original US machine code and discovery provenance.** This entry lies inside the direct-call function at `0x231E0`. Normal fallthrough is:

```text
0x23277  mov eax, [esi+0xC4]
0x2327D  cmp eax, 1
0x23280  jle 0x2328F
```

There is no incoming code xref. Two aligned raw occurrences map to `.data` addresses `0x21C644` and `0x21C658`. Matching-decomp identifies the containing array at `0x21C638` as `g_jetGraffitiScoresPerStage`, a `uint[6][13]`; neighbouring integers also happen to equal code addresses. They are scores, not function pointers.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** The named score table decisively rejects callable-table provenance, and no function symbol exists at `0x23280`.

**Video/research explanation.** No address-specific explanation was found. This is another concrete numeric-table collision.

**Our hypothesis.** The standalone entry is D. Its real `JLE` belongs to the `0x231E0` owner and consumes the immediately preceding `CMP`.

### `0x000272F7` — B, high confidence

**Original US machine code and discovery provenance.** This is the last target in the 45-word switch table at `0x2730C`, whose entries range across the larger state handler beginning before `0x267B7`. `0x272F7` clears `[esi+0x1640]`, sets `eax=1`, and tail-jumps to the owner's common continuation at `0x2680B`. The vtable scanner mislabels the in-code switch table as a `game_vtable`.

The two unresolved markers occur only because the frozen generated extent decodes the switch-table words after the real tail jump. They are not original instructions on the case path; the current disassembly correctly records only four instructions before the table.

**Observed runtime behaviour.** `0x272F7` appears in the preserved build's indirect-call target dump, confirming that the table case was selected in a captured run. No exact false-marker execution was found.

**Matching-decomp reconstruction.** No independent function symbol exists at `0x272F7`.

**Video/research explanation.** No address-specific explanation was found. Existing boundary research classifies switch cases that tail into a shared owner as internal continuations.

**Our hypothesis.** The entry is B. It is a real indirect-jump case, while both diagnostics are jump-table decoding artifacts.

### `0x00027C3E` — B, high confidence

**Original US machine code and discovery provenance.** Five conditional branches from `0x27B12`, `0x27B46`, `0x27B5F`, `0x27BE5`, and `0x27BFA` converge on this common return path:

```text
0x27C3E  xor eax, eax
0x27C40  pop esi
0x27C41  add esp, 0x100
0x27C47  ret
```

The words from `0x27C48` through `0x27C6C` are a ten-entry jump table containing `0x27B1F`, `0x27B56`, `0x27B7D`, `0x27BF1`, `0x27C27`, and other internal cases. The generated alias decodes that table until the independent function at `0x27C70`; its `JNP`, `JL`, `JNP`, and `JNP` markers are pointer-byte artifacts after the real return.

**Observed runtime behaviour.** No exact marker or indirect-call observation was found. The five direct machine-code edges establish the continuation.

**Matching-decomp reconstruction.** No independent function symbol exists at `0x27C3E`.

**Video/research explanation.** No address-specific explanation was found. The documented shared-epilogue model applies.

**Our hypothesis.** The entry is B. It is a legitimate shared return block; all four diagnostics should disappear when the extent stops before the jump table.

## Cumulative classification after batch 6

| Class | Functions |
|---|---:|
| A | 12 |
| B | 7 |
| C | 0 |
| D | 11 |
| E | 0 |
| Not yet classified | 12 |

## Batch 7 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x0003401E` | 1 | B | Switch-table bytes decoded beyond a real case tail | High |
| `0x00035006` | 1 | B | Switch/data bytes decoded beyond a real case tail | High |
| `0x00035D1F` | 1 | B | Switch/data bytes decoded beyond a real case tail | High |
| `0x00038000` | 11 | D | Entire entry is embedded jump/configuration data | High |
| `0x00040AAC` | 1 | D | Real owner branch at a numeric encoding-table split | High |

### `0x0003401E`, `0x00035006`, and `0x00035D1F` — B, high confidence

**Original US machine code and discovery provenance.** Each address is a real terminal case in a large state-machine switch:

- `0x3401E` clears `[ebx+0x167C]`, sets `eax=1`, and tail-jumps to `0x33D18`; it is the final entry of the jump table at `0x34034`.
- `0x35006` clears `[ebx+0x1618]`, sets `eax=1`, and tail-jumps to `0x348E9`; it is an entry in the jump table at `0x35018`.
- `0x35D1F` clears `[esi+0x16AC]`, sets `eax=1`, and tail-jumps to `0x356CB`; it is the final entry of the jump table at `0x35D34`.

The vtable scanner labels all three in-code jump tables as `game_vtable`. The real case paths terminate after four, four, and three instructions respectively. Their generated aliases instead decode the following tables and packed configuration bytes, producing one false unresolved marker apiece (`LOOP`, `JA`, and `JA`).

**Observed runtime behaviour.** No exact case-target or marker observation was found in the available dumps. The aligned tables and tail jumps establish their static internal-control-flow role.

**Matching-decomp reconstruction.** No independent symbols exist at these addresses. The reconstruction provides no competing callable boundaries.

**Video/research explanation.** No address-specific explanation was found. Existing state-machine and boundary research treats table-selected cases that tail back into a common owner as internal continuations.

**Our hypothesis.** All three entries are B. Their diagnostics are data decoding after terminal case jumps, not unresolved predicates in those case blocks.

### `0x00038000` — D, high confidence

**Original US machine code and discovery provenance.** The address itself is embedded data between a terminal switch case at `0x37F9E` and the next real function at `0x38090`. Its first words are addresses in the preceding state machine:

```text
0x38000: 0x37AA6 0x37AB0 0x37B02 0x37B7B
0x38010: 0x37BFB 0x375E9 0x37C37 0x37C4B
...
0x38040: 0x37F9E 0x37620 0x37627 0x3762C
```

These are followed by compact byte configuration arrays. Decoding the whole region as x86 creates eleven unresolved conditional opcodes.

The scanner's seed came from `.data` address `0x22AB68`, but that source is a geometric bit-value table (`0xE000`, `0x1C000`, `0x38000`, `0x70000`, then successively shifted values), not a function-pointer table. Numerous other raw occurrences likewise lie in packed numeric/graphic data.

**Observed runtime behaviour.** No entry, indirect-call, or marker observation was found.

**Matching-decomp reconstruction.** No function symbol exists at `0x38000`.

**Video/research explanation.** No address-specific explanation was found. Both the target bytes and the discovery source independently identify data.

**Our hypothesis.** The entry is D. All eleven markers are false instructions and require data exclusion, not flag recovery.

### `0x00040AAC` — D, high confidence

**Original US machine code and discovery provenance.** This address lies inside the direct-call function at `0x406D0`, with no incoming code xref. The real branch consumes the owner's comparison:

```text
0x40AA6  cmp dword ptr [0x251D58], edi
0x40AAC  je  0x40AC5
```

Its only raw address occurrence maps to `.rdata` at `0x1C42BC`. Matching-decomp identifies the enclosing array at `0x1C4248` as `D3D8::D3DSIMPLERENDERSTATEENCODE[82]`. Neighbouring values advance by four (`0x40AA0`, `0x40AA4`, `0x40AA8`, `0x40AAC`, `0x40AB0`, ...), proving they are numeric encodings rather than callable pointers.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** The named encoding array supplies decisive discovery provenance; no function symbol exists at `0x40AAC`.

**Video/research explanation.** No address-specific explanation was found. The monotone numeric-table collision model applies.

**Our hypothesis.** The standalone entry is D. Its `JE` is a real owner-path branch whose immediately preceding `CMP` was cut away by the artificial split.

## Cumulative classification after batch 7

| Class | Functions |
|---|---:|
| A | 12 |
| B | 10 |
| C | 0 |
| D | 13 |
| E | 0 |
| Not yet classified | 7 |

## Batch 8 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x00041E24` | 1 | D | Real owner branch at a numeric encoding-table split | High |
| `0x00041E48` | 1 | D | Same real owner branch after a later artificial split | High |
| `0x00041E4C` | 1 | D | Same real owner branch after a later artificial split | High |
| `0x00041E50` | 1 | D | Same real owner branch after a later artificial split | High |
| `0x00041E54` | 1 | D | Same real owner branch at the branch instruction | High |

### `0x00041E24` — D, high confidence

**Original US machine code and discovery provenance.** This lies inside the direct-call function at `0x41BF0`. The real `JGE` at `0x41E30` uses flags from the owner's `TEST edx,edx` at `0x41E22`; the intervening x87 load/store and `MOV` do not define integer EFLAGS.

The exact raw occurrence maps to `.rdata` address `0x1C42F8`, inside matching-decomp's `D3D8::D3DSIMPLERENDERSTATEENCODE[82]`. This region is an encoding array whose neighbouring values enumerate instruction-aligned offsets such as `0x41E20`, `0x41E24`, `0x41E40`, and `0x41E44`. It is not a callable table.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** The named encoding array establishes the discovery source; no independent function symbol exists at `0x41E24`.

**Video/research explanation.** No address-specific explanation was found. The same D3D numeric-encoding collision demonstrated at `0x40AAC` applies.

**Our hypothesis.** The standalone entry is D. Its real branch remains part of the `0x41BF0` owner's integer-to-float conversion sequence.

### `0x00041E48`, `0x00041E4C`, `0x00041E50`, and `0x00041E54` — D, high confidence

**Original US machine code and discovery provenance.** These four nested splits all lie in the same owner and lead to the same real branch:

```text
0x41E40  and  ecx, 0xFF
0x41E46  test ecx, ecx
0x41E48  mov  [esp+0x10], ecx
0x41E4C  fstp [esp+0x54]
0x41E50  fild [esp+0x10]
0x41E54  jge  0x41E5C
```

The four exact raw occurrences are consecutive words at `.rdata` addresses `0x1C4304`, `0x1C4308`, `0x1C430C`, and `0x1C4310`, again inside `D3D8::D3DSIMPLERENDERSTATEENCODE[82]`. None has an incoming code xref or separate prologue.

Each generated alias reports the same unresolved `JGE` because its start omits the `TEST ecx,ecx` at `0x41E46`. The x87 and move instructions between the producer and consumer preserve integer flags.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found for any of the four.

**Matching-decomp reconstruction.** The reconstruction identifies the source array as numeric encodings and provides no independent function boundary for any address.

**Video/research explanation.** No address-specific explanation was found. The address-taken research's structural-validation rule rejects these monotonically spaced numeric values.

**Our hypothesis.** All four standalone entries are D. They are multiple scanner cuts through one real flag-dependent owner sequence, not four separate behavioural defects.

## Cumulative classification after batch 8

| Class | Functions |
|---|---:|
| A | 12 |
| B | 10 |
| C | 0 |
| D | 18 |
| E | 0 |
| Not yet classified | 2 |

## Batch 9 result

| Function | Markers | Entry class | Marker status | Confidence |
|---|---:|---|---|---:|
| `0x00051402` | 1 | D | Real owner branch at a false DOLBY-data split | High |
| `0x00052402` | 1 | D | Real owner branch at a false DOLBY-data split | High |

### `0x00051402` — D, high confidence

**Original US machine code and discovery provenance.** This is the tail of the preceding routine. Its real branch uses the immediately preceding x87-status test:

```text
0x513FD  fnstsw ax
0x513FF  test   ah, 1
0x51402  jne    0x51417
```

There is no incoming code xref. The scanner seed is DOLBY address `0x282B24`, surrounded by mixed audio-table values including `0x20000B`, `0x50C03`, `0xD1080`, `0xFFFC33`, and small constants. It is not a validated callable table.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** No independent symbol exists at `0x51402`; the next matching method, `CMissionChild3Child1::DrawListEvent`, begins at `0x51420` after this owner's return and padding.

**Video/research explanation.** No address-specific explanation was found. The DOLBY mixed-data false-positive model applies.

**Our hypothesis.** The standalone entry is D. Its `JNE` is genuine owner-path behaviour and must consume `TEST ah,1`; the scanner-created boundary is not callable.

### `0x00052402` — D, high confidence

**Original US machine code and discovery provenance.** This lies inside matching-decomp's `CMission::InitResources` at `0x52350`. Its real equality branch follows the owner's comparison directly:

```text
0x523FC  mov ebp, [eax+4]
0x523FF  cmp ebp, [ecx+4]
0x52402  je  0x5240D
```

No code xref enters `0x52402`. The address occurs five times in DOLBY data; the scanner-selected occurrence at `0x282388` is embedded among mixed small constants, audio-format-like values, and nearby near-collisions such as `0x52403`, `0x52404`, and `0x52407`. This is structured data, not a C++ vtable.

**Observed runtime behaviour.** No exact entry, indirect-call, or marker execution was found.

**Matching-decomp reconstruction.** The matching `CMission::InitResources` boundary at `0x52350` contains the branch and continues to the next named method at `0x52460`; no independent symbol exists at `0x52402`.

**Video/research explanation.** No address-specific explanation was found. The DOLBY collision evidence and matching owner boundary agree.

**Our hypothesis.** The standalone entry is D. The `JE` is a genuine branch in `CMission::InitResources`, not an independent entry contract.

## Final classification

| Class | Functions | Share |
|---|---:|---:|
| A — genuine independent callable | 12 | 28.6% |
| B — legitimate internal continuation | 10 | 23.8% |
| C — recovery-created mid-function | 0 | 0.0% |
| D — questionable/artificial discovery | 20 | 47.6% |
| E — insufficient evidence | 0 | 0.0% |
| **Total** | **42** | **100.0%** |

All **68 markers** are accounted for. The dominant causes are not equivalent:

- real owner-path branches whose flag producer was cut away by an artificial entry;
- real branches from later independent functions pulled into overlong table-derived aliases;
- decoded jump-table, configuration-table, bitmap, or string bytes that are not instructions;
- legitimate internal continuations whose entry state depends on their actual incoming control-flow edges;
- five genuine mixed-width ZF joins in the independently callable function at `0x130FD0`.

No audited address required class C or E after raw provenance, matching boundaries, and the available runtime observations were combined. This does not mean every class-A function or class-B continuation was exercised by the captured runs; absence of runtime evidence was kept distinct from negative reachability proof.

## Audit conclusion

The frozen count of 68 unresolved markers must not be treated as 68 uniform translator defects. Most are boundary or data-classification artifacts. The actionable translator/control-flow case is the five-site mixed-width ZF merge in `0x130FD0`; genuine internal continuations also require edge-correct state rather than an invented universal entry flag. Artificial entries should be removed or folded into their owner while retaining correct translation of any real owner-path branch.

The preserved runnable build and translator source remain untouched by this audit. Any remediation should be performed separately, then regenerated and compared against this evidence ledger.
