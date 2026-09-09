# QWEN_UNRESOLVED_FLAGS_AUDIT_SALVAGE

## Purpose

This document reconstructs the unresolved-flags provenance audit from evidence already captured in the conversation after the original Qwen session became repetitive and unstable.

It is intentionally conservative:

- confirmed facts are separated from inference;
- incomplete provenance is left incomplete;
- no A–E class is invented where the evidence is insufficient;
- this is documentation only and does not modify translator code, generated code, or the preserved runnable build.

---

## 1. Original audit goal

Audit the remaining unresolved flag-dependent branches in the fresh JSRF translation and determine what kind of entry each unresolved marker belongs to.

The intended classifications were:

- **A — genuine independent callable entry**
- **B — legitimate internal jump/continuation entry**
- **C — recovery-created mid-function entry**
- **D — questionable/artificial discovery**, including likely scanner/data false positives
- **E — unknown / insufficient evidence**

The purpose was specifically to avoid treating every unresolved marker as the same kind of translator bug.

Priority addresses were:

- `0x0005089B`
- `0x000749DC`
- `0x000800E1`

The audit was read-only.

---

## 2. Confirmed audit population

The isolated mixed-ZF fresh generation contains:

- **68 unresolved flag branches**
- across **42 generated functions**

Measured branch-type distribution:

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
| `jp` | 2 |
| `jle` | 2 |
| `jae` | 2 |
| `jb` | 1 |
| `jl` | 1 |
| `loop` | 1 |
| **Total** | **68** |

A separate generated tree later showed **72 probes at 45 unique addresses**. That is not the same audit population and must not replace the confirmed **68 / 42** baseline.

---

## 3. Important audit-wide finding

The remaining 68 markers are **not 68 equivalent translator defects**.

Evidence already distinguishes at least three materially different cases:

1. a real owner-path condition whose remaining standalone entry is probably artificial (`0x5089B`);
2. a genuine machine-code continuation with origin-dependent flags (`0x749DC`);
3. a generated entry with no demonstrated incoming provenance (`0x800E1`).

Therefore the unresolved-marker count must be classified by provenance before further translator fixes are attempted.

---

## 4. Function-discovery population

The disassembler summary contained these major discovery populations:

| Detection method | Count |
|---|---:|
| `call_target` | 4,089 |
| `tail_jump_alias` | 3,775 |
| `seed_vtable_thunk` | 1,871 |
| `prologue` | 441 |
| `imm_ref_target` | 117 |
| `tail_jump_target` | 97 |
| `indirect_call_slot` | 8 |
| `cc_boundary` | 5 |
| `entry_point` | 1 |

### Confirmed implication

A generated function start is **not by itself proof of a conventional independently callable function**.

Large parts of the generated function population consist of aliases, continuation-like starts, scanner-created starts, or recovery/discovery constructs.

---

## 5. Existing provenance datasets checked

The unresolved-address set was checked against existing datasets including:

- `shared_epilogues.json`
- `midfunction_entries.json`
- startup-entry data
- icall-entry data
- available `icall_targets.dump`

For the priority addresses `0x5089B`, `0x508DF`, `0x749DC`, and `0x800E1`, no icall observation was found in the searches performed.

The priority addresses were also not established by the checked recovery/startup/shared-epilogue lists as known entries of those classes.

This is a negative result about the available evidence only. It is not proof that indirect runtime entry is impossible.

---

# CONFIRMED FACTS

## 6. `0x0005089B`

### Machine-code evidence

Raw XBE bytes at `0x5089B` begin:

```text
0f 84 7f 01 00 00 ...
```

Therefore the address starts directly with a flag-dependent `JE`.

The disassembly separates:

- `sub_00050860`
- `sub_0005089B`
- `sub_000508DF`

No normal incoming code xref to `0x5089B` was found during the audit.

### Scanner provenance

`vtable_seeds_accum.json` records:

```json
{
  "start": "0x0005089B",
  "category": "game_vtable",
  "subcategory": "cls_708",
  "confidence": 0.75,
  "method": "vtable_thunk",
  "vtable_addr": "0x0027E448",
  "vtable_index": 0,
  "source": "func_id-vtable-scanner"
}
```

This establishes that at least one reason `0x5089B` became an independent generated entry is the vtable scanner.

### Actual XBE data at `0x0027E448`

The raw XBE words read from the alleged table were:

```text
0x5089b
0x50c85
0x7da98
0x50914
0x50c82
0x7f084
0x116
0xbc448
0x77084
0x116
```

These values are confirmed XBE data.

The claim that the structure is a `game_vtable` is the **scanner's interpretation**, not an independently established fact.

The structure contains small non-code-like values such as `0x116` among code-like values.

It was **not independently validated as a genuine MSVC C++ vtable** during the audit.

### Canonical decompilation evidence

The canonical JSRF decompilation symbol table identifies:

```text
0x00050860  CCharacterSelect_child::Exec0Default
```

It also identifies:

```text
0x001CAC60  CCharacterSelect_child::`vftable'
```

Raw XBE words inspected at the independently named `0x001CAC60` vtable begin:

```text
0x55d90
0x50860
0x11c90
0x50a30
0x11c90
0x11c90
0x11c80
0x11c90
```

This is a structurally credible positive control linking `0x50860` to the class.

No canonical decompilation symbol for `0x5089B` was found in the searches performed.

### Previous translator evidence

Earlier focused translator analysis had already repaired the real owner path involving this region.

That work established that the owner path contained a valid comparison and that the generated owner function could safely use the proven predicate.

The unresolved standalone `0x5089B` marker remained after that owner-path repair.

### Provisional classification

**D — questionable/artificial discovery**

**Confidence: high**

### Rationale

The strongest interpretation is that standalone `0x5089B` is likely a scanner-created/artificial boundary inside code whose real behavioural owner is `0x50860`.

Supporting evidence:

- begins directly with `JE`;
- no incoming code xref found;
- no icall observation found;
- scanner provenance comes from a suspicious unvalidated table;
- canonical decomp identifies the owner method at `0x50860`;
- canonical class vtable points to `0x50860`, not `0x5089B`;
- the real owner path has already been repaired separately.

This does **not** prove the scanner seed must be removed; it does support treating the remaining standalone marker as different from a genuine unresolved behavioural branch.

---

## 7. `0x000508DF`

### Scanner provenance

`vtable_seeds_accum.json` records:

```json
{
  "start": "0x000508DF",
  "category": "game_vtable",
  "subcategory": "cls_715",
  "confidence": 0.75,
  "method": "vtable_thunk",
  "vtable_addr": "0x0027F414",
  "vtable_index": 1,
  "source": "func_id-vtable-scanner"
}
```

### Actual XBE data at `0x0027F414`

The raw XBE words read were:

```text
0x5a407
0x508df
0xd1080
0x7ba
0xd1080
0x7ed
0x50c05
0xd1080
0x7c0
0xd1080
```

Again:

- the words are confirmed XBE data;
- `game_vtable` is the scanner interpretation;
- the structure was not independently validated as a real C++ vtable during the audit.

No canonical decompilation symbol for `0x508DF` was found in the searches performed.

### Provisional classification

**E — unknown / insufficient evidence**

**Confidence: medium**

The scanner provenance is suspicious, but the audit did not complete enough independent provenance work to classify this entry as artificial.

---

## 8. `0x000749DC`

### Machine-code evidence

Raw XBE bytes at `0x749DC` begin:

```text
74 0e ...
```

Therefore the address starts directly with a flag-dependent `JE`.

### Incoming control-flow evidence

Unlike `0x5089B`, there is a real machine-code jump into this address:

```text
0x00074AEE  jmp 0x000749DC
```

The disassembly explicitly records this edge.

`0x749DC` was not found in the vtable-seed set.

No icall observation for `0x749DC` was found in the searched logs.

No canonical decompilation symbol for `0x749DC` was found in the searches performed.

### Earlier focused analysis

Previous bounded translator work concluded that the standalone entry cannot safely be assigned one universal inherited flag state.

Different legitimate origins can carry different relevant flag histories into the continuation.

An attempted generic repair by borrowing one predecessor's flags would therefore be unsafe.

### Provisional classification

**B — legitimate internal jump/continuation entry**

**Confidence: high**

### Rationale

The real `0x74AEE -> 0x749DC` machine-code jump proves that this address is not merely an orphan scanner discovery.

Its unresolved condition is instead an example of an internal continuation whose semantics depend on the incoming edge.

This likely requires an origin-specific translation mechanism such as edge splitting or duplicated continuation translation rather than a single standalone flag state.

No such broader mechanism was implemented during the bounded work.

---

## 9. `0x000800E1`

### Machine-code evidence

Raw XBE bytes at `0x800E1` begin:

```text
74 2a ...
```

Therefore the entry starts directly with a flag-dependent `JE`.

Its branch target is `0x8010D`.

### Dense generated-start cluster

The small region contained generated starts around:

- `0x800E1`
- `0x800E3`
- `0x800E9`
- `0x800EA`
- `0x800F0`
- `0x800F5`
- `0x800F9`
- `0x800FD`

The next clear function at `0x80110` was reported as a `call_target`.

### Xref evidence

Repeated xref investigation found no incoming xref to `0x800E1`.

A focused search for all xrefs involving the address returned only:

```text
0x000800E1 -> 0x0008010D  cond_jump
```

No incoming xrefs were found for the queried nearby generated starts either.

No corresponding references were found in the other disassembled sections searched.

`0x800E1` was not found in the vtable seed set.

No icall observation was found.

No canonical decompilation symbol for `0x800E1` was found.

### Provisional classification

**D — questionable/artificial discovery**

**Confidence: high-ish**

### Rationale

The combined evidence strongly suggests that `0x800E1` has not been demonstrated to be an independently callable source-level entry:

- starts with an inherited-flag branch;
- no incoming code/data xref found;
- no vtable seed;
- no icall observation;
- no decomp symbol found;
- exists inside a dense generated-start cluster.

The exact discovery mechanism that created the standalone entry was not established before the Qwen session began looping.

Therefore the stronger claim “proven false entry” is not justified.

---

## 10. Other confirmed xref observations

The audit explicitly established examples including:

- `0x001533D0`: no incoming xref found
- `0x00180038`: no incoming xref found
- `0x001BD800`: no incoming xref found
- `0x001C3800`: no incoming xref found
- `0x001C3F18`: one `data_imm` reference found

These should be preserved for the continuation of the audit.

---

## 11. Other confirmed vtable-seed hits

Among the unresolved-address population, confirmed entries found in `vtable_seeds_accum.json` included:

- `0x0005089B`
- `0x000508DF`
- `0x00051402`
- `0x00052402`
- `0x001028CF`
- `0x00134CEE`

Other hits were printed during the original session, but the complete clean list is not recoverable from the visible transcript with enough confidence to reproduce here.

A successor audit should reconstruct the complete list directly from repository data.

---

## 12. Vtable-scanner methodological finding

A major result of the audit is:

> Membership in `vtable_seeds_accum.json` is discovery provenance, not proof that an address is a real virtual method.

Three different levels of evidence must be kept separate:

1. **Raw XBE evidence**  
   A data word genuinely contains a code-like address.

2. **Scanner interpretation**  
   The surrounding data was classified by tooling as a vtable.

3. **Independent structural validation**  
   Constructor vfptr assignment, coherent table structure, known class symbol, canonical vtable symbol, or other corroborating evidence establishes that it is genuinely a function table/vtable.

`0x001CAC60 / CCharacterSelect_child::vftable` is a useful positive control.

`0x0027E448 / 0x5089B` and `0x0027F414 / 0x508DF` are suspect examples that were not independently validated.

---

## 13. Engine/decompilation constraint on provenance classification

The Basics of the Engine/decompilation cross-reference established that legitimate JSRF executable entries do not always require a direct `CALL` xref.

JSRF uses:

- virtual dispatch;
- object vtables;
- table-driven state functions;
- internal traversal/dispatch mechanisms.

Therefore:

> **No CALL/JMP xref is not sufficient evidence that an entry is artificial.**

A data/table-derived entry must be judged by whether the table itself is structurally credible and independently supported.

This prevents over-classifying all xref-less entries as D.

---

# STRONG INFERENCES

## 14. `0x5089B`

Likely a false-positive/artificial standalone boundary caused by scanner discovery.

The real behavioural owner is likely `0x50860`, whose path has already been repaired.

The remaining unresolved standalone diagnostic probably does not represent a remaining behavioural bug.

---

## 15. `0x749DC`

Likely a real internal continuation whose condition depends on the origin edge.

This class of problem cannot safely be solved by assigning one static fallback flag state to the standalone entry.

---

## 16. `0x800E1`

Likely an artificial/internal generated split rather than a demonstrated independent function entry.

The exact discovery source remains unfinished.

---

## 17. Scanner quality

Some `func_id-vtable-scanner` results likely need stronger structural validation before being trusted as independent function roots.

Pointer-like data alone is insufficient.

---

# UNRESOLVED / NOT CHECKED

## 18. Work not completed by the original audit

The following must not be inferred from this salvage record:

1. Complete A–E classification of all 42 functions.
2. Complete classification of all 68 branches.
3. Complete incoming-xref table for every unresolved function.
4. Complete data-reference table for every unresolved function.
5. Complete `vtable_seeds_accum` membership list for the 42-function population.
6. Complete canonical decomp symbol/object evidence for the population.
7. Exact discovery provenance of `0x800E1`.
8. Proof that `0x27E448` is definitely not a legitimate table of another kind.
9. Proof that `0x27F414` is definitely not a legitimate table of another kind.
10. Runtime reachability of every unresolved entry.
11. A safe generic translation mechanism for origin-dependent continuations such as `0x749DC`.
12. Any justification for suppressing unresolved diagnostics globally.
13. Any justification for deleting scanner-created entries without per-entry evidence.

---

## 19. Partial unresolved-function inventory recovered from the session

The conversation preserved this unresolved-function/address set used repeatedly during the audit:

```text
0x00014885
0x00023280
0x000272F7
0x00027C3E
0x0003401E
0x00035006
0x00035D1F
0x00038000
0x00040AAC
0x00041E24
0x00041E48
0x00041E4C
0x00041E50
0x00041E54
0x0005089B
0x000508DF
0x00051402
0x00052402
0x000749DC
0x000800E1
0x00100058
0x001000C9
0x001028CF
0x001237A0
0x00130FD0
0x00134CEE
0x00134CF3
0x00134CFD
0x00150130
0x00150170
0x001501F0
0x00150240
0x00150280
0x001502C0
0x00150C00
0x00150C70
0x00150CA0
0x001533D0
0x00180038
0x001BD800
0x001C3800
0x001C3F18
```

This is **42 addresses**, matching the confirmed 42-function audit population.

Unless specifically discussed elsewhere in this document, these entries should currently be treated as:

**NOT YET CLASSIFIED**

rather than assigned A–E by inference.

---

## 20. Current provisional classification table

| Function | Class | Confidence | Status |
|---|---|---:|---|
| `0x0005089B` | D | High | Provisionally classified |
| `0x000508DF` | E | Medium | Needs more provenance |
| `0x000749DC` | B | High | Provisionally classified |
| `0x000800E1` | D | High-ish | Exact discovery source unfinished |
| remaining 38 functions | — | — | Not yet classified |

---

## 21. Separate fresh-generation integrity problem

This audit must not be confused with the separate replacement-build integrity regression.

Previously established facts:

- `midfunction_entries.json` contains **208 recovered mid-function entries**;
- fresh generation regressed all 208 to stubs;
- **five registered startup/callback entries** were omitted;
- the recovery helper matcher was found to expect an older stub form and miss the newer logged stub format.

The preserved runnable build must **not** be replaced until that issue is repaired and independently validated.

This remains true regardless of how the 68 flag markers are classified.

---

## 22. Recommended continuation strategy

Do not rerun the entire audit as one long autonomous task.

Continue incrementally in batches of **five functions**.

For every function, persist immediately:

- function address;
- unresolved branch addresses and mnemonics;
- detection/discovery reason;
- direct incoming xrefs;
- data references;
- vtable-seed provenance;
- raw table evidence versus scanner interpretation;
- independent vtable/table validation;
- recovery/shared-epilogue/startup/icall membership;
- decomp symbol evidence;
- object-boundary evidence;
- whether the entry begins with a flag consumer before a local producer;
- known owner/predecessor;
- A–E class;
- confidence;
- unresolved questions.

Do not revisit a completed entry unless new evidence contradicts it.

If obvious provenance sources do not resolve an address, classify it **E** and move on instead of repeatedly searching the same evidence.

---

## 23. Suggested first incremental batch

Use the existing evidence rather than restarting these cases from scratch:

1. `0x0005089B`
2. `0x000508DF`
3. `0x000749DC`
4. `0x000800E1`
5. next pending unresolved function in ascending address order after `0x800E1`

For the first four, only verify the minimum necessary to turn the provisional classifications into durable audit entries.

Then stop and review before proceeding.

---

## 24. Handover to another model

The Qwen session produced useful provenance evidence but failed during the synthesis phase and became trapped repeatedly rechecking `0x800E1`.

Do **not** restart from zero.

Treat this document as the recovered audit state.

The next model should:

1. reconstruct the 68 branch instances directly from the isolated mixed-ZF output;
2. use the 42-address inventory above as the function-level checklist;
3. preserve the provisional findings for `0x5089B`, `0x508DF`, `0x749DC`, and `0x800E1`;
4. fill only missing evidence;
5. structurally validate table-derived entries rather than trusting scanner labels;
6. distinguish genuine table-driven entries from false-positive data scans;
7. classify five functions at a time;
8. persist each batch before continuing;
9. avoid translator changes until the provenance audit identifies an evidence-supported next repair;
10. leave the preserved runnable build untouched.

---

## Bottom line

The failed Qwen session did **not** invalidate the audit.

It established enough evidence to show that the remaining 68 unresolved branches are a heterogeneous population and that marker count alone is no longer a useful measure of remaining translator correctness.

The highest-confidence recovered conclusions are:

- `0x5089B`: likely artificial scanner-created standalone entry; real owner path already repaired.
- `0x749DC`: real internal continuation with origin-dependent flags.
- `0x800E1`: likely artificial/internal generated entry with no demonstrated incoming provenance.
- vtable-scanner seeds require independent structural validation.
- the remaining 38 functions still require systematic classification.
