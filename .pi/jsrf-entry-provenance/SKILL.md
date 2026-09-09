---
name: jsrf-entry-provenance
description: Determine why a JSRF generated function entry exists and distinguish real callable entries, internal continuations, recovery entries, and scanner-created boundaries.
---

# JSRF entry provenance

Use this skill to answer one narrow question:

**Why does this generated entry exist, and what evidence supports that provenance?**

Worktree:

`/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean`

Canonical decomp repository:

`/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/JSRF-Decompilation`

## Constraints

- READ ONLY unless the user explicitly asks for edits.
- Investigate one address at a time.
- Do not repair translator behavior.
- Do not broaden into runtime, renderer, or Metal work.
- Do not infer source-level function identity merely because a generated function starts at an address.

## Provenance categories to distinguish

A generated entry can originate from:

- entry point
- direct call target
- direct jump/tail-jump target
- tail-jump alias
- conditional-control-flow boundary
- prologue heuristic
- immediate/data reference target
- indirect-call slot/seed
- vtable scanner seed
- recovery-created mid-function entry
- shared epilogue
- startup/callback seed
- manual seed
- other discovery/recovery mechanism

The discovery label is evidence about tooling, not necessarily the true program semantics.

## Evidence hierarchy

Prefer, roughly:

1. actual machine-code incoming control-flow edge;
2. runtime indirect-call observation;
3. constructor/vfptr assignment and coherent table structure;
4. canonical symbol plus structurally matching binary evidence;
5. explicit recovery/startup/icall manifest membership;
6. discovery metadata;
7. raw data-pointer occurrence without validation;
8. absence of evidence.

Do not turn a low-level scanner label into a high-confidence semantic claim without corroboration.

## Vtable/function-table checks

When an address is table-derived:

1. Record the raw XBE words around the alleged table.
2. Record which tool called it a vtable/function table.
3. Look for:
   - constructor writes of the table address to `[this]`,
   - known class vtable symbols,
   - coherent method pointer layout,
   - plausible destructor/exec/draw slot structure,
   - multiple code pointers with sensible alignment/range,
   - absence/presence of suspicious small integers or mixed data.
4. State separately:
   - RAW DATA FACT
   - SCANNER INTERPRETATION
   - INDEPENDENT VALIDATION

## Important architectural constraint

JSRF legitimately uses:

- C++ virtual dispatch;
- table-driven state functions;
- internal continuation/tail-jump entries.

Therefore:

**No direct CALL/JMP xref is not sufficient to call an entry fake.**

Conversely:

**A data pointer to an address is not sufficient to call it a genuine function.**

## Known positive control

`CCharacterSelect_child::vftable` at `0x001CAC60` and method `CCharacterSelect_child::Exec0Default` at `0x00050860` are useful examples of independently corroborated table/class evidence.

## Required output

Return a concise provenance report:

```text
ADDRESS:
GENERATED DISCOVERY:
DIRECT CONTROL-FLOW:
INDIRECT/RUNTIME EVIDENCE:
DATA/TABLE EVIDENCE:
RECOVERY/SEED MEMBERSHIP:
DECOMP EVIDENCE:
MOST LIKELY PROVENANCE:
CONFIDENCE:
WHY:
WHAT IS NOT PROVEN:
```

Stop after the requested address.
