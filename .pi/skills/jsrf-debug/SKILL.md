---
name: jsrf-debug
description: Evidence-driven debugging and repair workflow for the JSRF static recompilation project. Use for runtime crashes, translation defects, flag propagation, Xbox/D3D/NV2A behaviour, renderer work, and regression investigation.
---

# JSRF Debugging

You are working on the Jet Set Radio Future static recompilation project.

## Core rule

Do not repair a suspected defect until evidence demonstrates the mechanism.

Prefer one proved fix over ten speculative fixes.

## Before changing code

1. Read the current goal/handover file named by the user.
2. Inspect `git status`.
3. Identify the exact failing site or behaviour.
4. Find the generated code, source implementation, trace, or machine-code evidence involved.
5. State:
   - observed fact
   - interpretation
   - proposed experiment

Do not treat an interpretation as an observed fact.

## Investigation discipline

Work on one causal question at a time.

Prefer experiments that discriminate between hypotheses.

Examples:

- compare generated translation with Xbox machine code
- trace one branch or call site
- inspect all predecessors of a flag-consuming branch
- compare successful and failing runtime paths
- measure pushbuffer/method behaviour
- reproduce before editing

Do not roam through unrelated TODOs simply because they are visible.

## Editing

Make the smallest change that fixes the demonstrated mechanism.

Do not:
- rewrite unrelated code
- perform opportunistic cleanup
- suppress assertions merely to progress
- replace unknown behaviour with arbitrary constants
- mark unresolved behaviour as solved without evidence
- modify multiple suspected sites simultaneously

When generated files are involved, fix the generator/backport mechanism where appropriate rather than silently editing generated output.

## Validation

After a change:

1. Run the narrowest relevant test.
2. Reproduce the original failure.
3. Run the appropriate regression tests.
4. Compare measurable behaviour before and after.

A later crash is not automatically proof that the previous fix is correct.

## Runtime debugging

When execution progresses farther, record:

- previous failure point
- new failure point
- relevant registers/state
- call path
- whether behaviour is deterministic
- whether existing tests still pass

## Flag translation defects

For `_flags` / CMP / TEST / conditional-branch problems:

1. Locate every predecessor reaching the branch.
2. Compare original Xbox instructions.
3. Determine which instruction actually supplies flags.
4. Confirm operand width and signedness.
5. Determine whether paths have compatible flag-producing semantics.
6. Only then use a snapshot/merge/backport mechanism.

Never infer flag provenance solely from generated block order.

## Renderer/NV2A work

Separate:

- parser correctness
- method decoding
- render-state modelling
- fixed-function transformation
- texture stages/combiners
- rasterisation/presentation

Do not use successful pushbuffer parsing as evidence that rendering semantics are correct.

## Stop conditions

Stop expanding the investigation when:

- the requested question has been answered
- the proposed mechanism is disproved
- evidence is insufficient for a safe edit
- another independent blocker has been reached

At that point report rather than improvising.

## Final report

Always finish with:

### Verified
What the evidence demonstrates.

### Changed
Exactly what was modified, or "nothing".

### Validation
Commands/tests/runtime observations.

### Remaining
The next unresolved blocker.

### Next
One recommended next experiment.
