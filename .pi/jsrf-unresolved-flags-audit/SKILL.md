---
name: jsrf-unresolved-flags-audit
description: Audit one JSRF unresolved-flags generated entry at a time using bounded provenance checks and return a compact A-E classification record.
---

# JSRF unresolved-flags audit

Use this skill only for the unresolved-flags provenance audit in:

`/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean`

This is an evidence-gathering task, not a translator-fixing task.

## Hard constraints

- READ ONLY.
- Do not modify translator code.
- Do not modify generated code.
- Do not regenerate the project.
- Do not touch the preserved runnable build.
- Do not investigate renderer, Metal, graphics, audio, or unrelated runtime issues.
- Audit exactly ONE function address per invocation.
- Do not create or edit audit Markdown files unless the user explicitly asks for file writing.
- Do not investigate another function after the requested one.
- Stop after returning the compact record.

## Audit population

The canonical isolated mixed-ZF audit population is:

- 68 unresolved flag branches
- across 42 generated functions

Do not treat all 68 markers as equivalent translator defects.

## Classification taxonomy

Use exactly one:

- **A** — genuine independent callable entry
- **B** — legitimate internal jump/continuation entry
- **C** — recovery-created mid-function entry
- **D** — questionable/artificial discovery, including likely scanner/data false positive
- **E** — unknown / insufficient evidence

Never force a classification. Use E when the evidence remains weak.

## Core rules

1. No incoming CALL/JMP does **not** prove an entry is artificial. JSRF uses virtual dispatch and table-driven state functions.
2. A `vtable_seeds_accum.json` record is discovery provenance, not proof of a genuine vtable.
3. Distinguish:
   - raw XBE words,
   - scanner interpretation,
   - independent structural validation.
4. Do not borrow flags from an arbitrary predecessor unless all legitimate incoming origins prove the same relevant flag state.
5. If different legitimate origins carry different flag histories, record that explicitly.
6. If a negative search result has already been established for this address, do not repeat it unless contradictory evidence appears.
7. Use the obvious evidence sources once. If provenance remains unclear, classify E and stop.
8. Prefer direct machine-code/xref evidence over inferred naming.
9. Canonical JSRF-Decompilation symbols are corroborating evidence; absence of a symbol is not proof of artificiality.
10. A generated function boundary may be a call target, tail-jump alias, recovery entry, scanner seed, or other discovery construct rather than a C++ source-level function.

## Evidence sources

Use only what is needed for the requested address.

Primary worktree evidence commonly includes:

- `build-macos/jsrf-first-fault/flags-comparison/mixed-zf/gen/`
- `diagnostics/jsrf_first_fault/audit_unresolved_flags.py`
- `build-macos/jsrf-first-fault/disasm/functions.json`
- `build-macos/jsrf-first-fault/disasm/xrefs.json`
- `build-macos/jsrf-first-fault/disasm/asm/text.asm`
- `diagnostics/jsrf_first_fault/midfunction_entries.json`
- `diagnostics/jsrf_first_fault/shared_epilogues.json`
- `diagnostics/jsrf_first_fault/startup_entries.json`
- `diagnostics/jsrf_first_fault/icall_entries.json`
- `diagnostics/jsrf_first_fault/icall_seed.json`
- `diagnostics/jsrf_first_fault/icall_targets.dump` if present
- `vtable_seeds_accum.json` wherever present in the established diagnostics/build tree

Canonical decompilation repository:

`/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/JSRF-Decompilation`

Useful evidence:

- `ghidra/symboltable.tsv`
- `objects.csv`
- source/class declarations when relevant

## Positive-control principle

If an alleged vtable/table is important to classification, compare its structure with a known coherent table when useful.

Known example:

- `CCharacterSelect_child::vftable` at `0x001CAC60`
- known class method `CCharacterSelect_child::Exec0Default` at `0x00050860`

Do not assume every scanner-labelled pointer block is equivalent to this.

## Known prior leads

Treat these as prior evidence to verify minimally, not invitations for broad re-investigation.

### 0x0005089B

Previously established:

- begins directly with `JE`
- no normal incoming code xref found
- scanner seed from `func_id-vtable-scanner`
- alleged table at `0x0027E448`
- raw words included:
  `0x5089b, 0x50c85, 0x7da98, 0x50914, 0x50c82, 0x7f084, 0x116, ...`
- canonical decomp identifies `0x50860` as `CCharacterSelect_child::Exec0Default`
- canonical decomp identifies class vtable at `0x001CAC60`
- owner-path condition in `0x50860` has already been repaired
- standalone `0x5089B` remains suspicious as an artificial/scanner-created boundary

Likely class: D, but confirm only enough evidence to support the record.

### 0x000749DC

Previously established:

- begins directly with `JE`
- real incoming jump `0x74AEE -> 0x749DC`
- `functions.json` discovery method: `tail_jump_alias`
- no vtable seed found
- no icall observation found
- prior analysis found origin-dependent flag history

Likely class: B.

### 0x000800E1

Previously established:

- begins directly with `JE`
- branch target `0x8010D`
- no incoming xref found
- no vtable seed found
- no icall observation found
- no canonical decomp symbol found
- dense generated-start cluster around `0x800E1` through `0x800FD`
- next clear call target at `0x80110`

Likely class: D or E depending whether the actual discovery source can be established cheaply.

## Procedure

For the ONE requested function:

1. Identify every `UNRESOLVED FLAGS` branch in that generated function.
2. Record branch address and mnemonic.
3. Read the function's discovery method from `functions.json`.
4. Check direct incoming code xrefs in `xrefs.json`.
5. Check relevant data/immediate references.
6. Check vtable-seed membership and provenance.
7. If table-derived, separate raw table contents from scanner interpretation and independent validation.
8. Check membership in:
   - midfunction entries,
   - shared epilogues,
   - startup entries,
   - icall lists/observations.
9. Check canonical decomp symbol evidence.
10. Check canonical object-boundary evidence if useful.
11. Determine whether the generated entry consumes flags before any local flag producer.
12. Identify a known owner/predecessor if evidence supports one.
13. Classify A/B/C/D/E.
14. Stop.

## Required response format

Return exactly one compact record:

```text
FUNCTION:
UNRESOLVED BRANCHES:
DISCOVERY METHOD:
INCOMING CODE XREFS:
DATA REFS:
VTABLE SEED:
TABLE VALIDATION:
MIDFUNCTION ENTRY:
SHARED EPILOGUE:
STARTUP ENTRY:
ICALL EVIDENCE:
DECOMP SYMBOL:
OBJECT-BOUNDARY EVIDENCE:
FLAGS AT ENTRY:
KNOWN OWNER/PREDECESSOR:
CLASS: A/B/C/D/E
CONFIDENCE:
EVIDENCE:
OPEN QUESTION:
```

Do not append a plan, suggested next function, translator fix, or general summary.
