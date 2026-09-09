---
name: jsrf-decomp-crossref
description: Cross-reference a JSRF US-XBE address against the canonical JSRF-Decompilation repository without overclaiming from symbols or object ranges.
---

# JSRF decomp cross-reference

Use this skill to cross-reference one or a small bounded set of US-XBE addresses against:

`/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/JSRF-Decompilation`

The active recompilation binary remains authoritative.

## Constraints

- READ ONLY.
- Do not change decomp or recompilation files.
- Do not assume symbol absence means non-code.
- Do not assume symbol presence proves runtime reachability.
- Do not silently translate names from video terminology into decomp names without evidence.
- Preserve exact addresses and exact symbol names.

## Main sources

Start with:

- `ghidra/symboltable.tsv`
- `objects.csv`

Then inspect source only when needed.

Useful source areas may include:

- `decompile/src/JSRF/`
- `decompile/src/XDK/`
- headers defining classes/vtables/structures

## Interpretation rules

### `symboltable.tsv`

A symbol hit can provide:

- function/data identity
- calling convention
- class/method association
- vtable name
- matching/nonmatching/unimplemented status if encoded nearby

Treat it as strong corroboration, but the active US XBE bytes are still authoritative.

### `objects.csv`

Use object ranges as supporting evidence for:

- likely original object/module ownership
- whether nearby known symbols belong to the same compiled unit

Absence from object coverage is not proof that an address is artificial; coverage may be incomplete.

### Constructors and vtables

When available, constructors are especially strong for physical object layout and vfptr validation.

Known proven `CActBase` layout in the active XBE:

```text
+00 vfptr
+04 flags
+08 action ID
+0C draw-child mask
+10 depth/Z
+14 sort key
+18,+1C,+20 translation[3]
+24 parent
+28 first child
+2C previous sibling
+30 next sibling
+34 draw-next
+38 draw backlink
+3C draw-list tail anchor
+40 sorted-draw link
```

Base size: `0x44`.

Known base vtable: `0x001C4390`.

The 16-entry layout is destructor plus 15 mode methods:

```text
+00 destructor
+04 Exec0Default
+08 Exec1Default
+0C DrawDefault
+10 Exec0Event
+14 Exec1Event
+18 DrawEvent
+1C Exec0CoveredPause
+20 Exec1CoveredPause
+24 DrawCoveredPause
+28 Exec0FreezeCam
+2C Exec1FreezeCam
+30 DrawFreezeCam
+34 Exec0UncoveredPause
+38 Exec1UncoveredPause
+3C DrawUncoveredPause
```

This is a useful structural reference when evaluating alleged JSRF vtables.

## Terminology cross-reference

The decomp/video naming correspondence previously established is approximately:

- video `Game` ≈ decomp `CActMan`
- video `GameObj` ≈ decomp `CActBase`

Do not assume other video names, such as Director, have direct literal symbol matches.

## Required output

For each requested address:

```text
ADDRESS:
SYMBOLTABLE:
OBJECT RANGE:
SOURCE HIT:
CLASS/MODULE:
STRUCTURAL EVIDENCE:
INTERPRETATION:
LIMITS:
```

If there is no hit, say exactly which sources were checked and report `NO HIT FOUND`; do not infer more than that.
