# Order-independent backward-edge flag analysis

Implemented a translator fix for the demonstrated 0x15275 CFG shape. The previous single address-ordered emission pass could not use a flag setter located later in the function. It emitted a constant-zero fallback even when both incoming edges contained compatible 32-bit comparisons.

## Implementation

- tools/recomp/lifter.py: extracted the existing instruction flag-transfer rules into a pure helper shared with CFG analysis, avoiding speculative lifting and its code-generation side effects.
- tools/recomp/translator.py: computes reaching flag definitions with a worklist to a fixed point before emitting blocks. Empty definition sets (not yet reached) differ from unknown entry/clobbered flags. Finite unions converge through loops. Only after propagation does the existing compatibility merge determine the incoming state.
- tools/recomp/test_backward_flag_flow.py: six regressions covering actual byte translation of the two-comparison backward join, block-order independence, preserving loops, unknown entry paths, incompatible setters/widths, and undefined flags on a predecessor. The fixture restores global translation configuration after each test.

This is intra-function CFG analysis. It does not carry local flags across separate generated C functions, and it deliberately leaves mixed CMP/TEST joins unresolved. Existing instruction flag semantics are retained rather than introducing new assumptions about calls or partially defined flags.

## Evidence

Fresh single-function translation from our US XBE now emits:

```
loc_00015275: ;
    if (CMP_NE(_fa, _fb)) goto loc_000153BC;
```

The mixed CMP/TEST branch at 0x153A9 still emits UNRESOLVED FLAGS. Its separately verified preserved-build backport remains necessary.

The new actual-byte regression fails against the HEAD baseline translator loaded in memory, where the shared JNE is still the constant-zero fallback, and passes against the changed translator. No baseline files were restored over the worktree.

## Validation

- Focused flag/merge tests: 20 passed.
- unittest discovery: 119 passed.
- Broad pytest invocation: 195 passed, one test_config failure because earlier tests changed process-global configuration (reported origin cfg-recovery-test).
- Separate-process validation: test_config.py 5 passed; remaining recompiler suite 191 passed, 41 subtests passed. Total 196 passing tests when the configuration test's pristine-process assumption is respected.
- Re-ran six new tests after adding configuration restoration: all passed.
- git diff --check passed.

No preserved JSRF runtime sources were regenerated or changed by this translator task, so no new game-run claim is made. The preserved-source 76-site count is unchanged; a fresh whole-title translation would be needed to measure the wider reduction and is intentionally outside this bounded fix.
