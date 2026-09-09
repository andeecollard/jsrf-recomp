# Mixed CMP/TEST joins

The translator now represents a mixed CMP/TEST reaching-definition set as a partial flag state: ZF is known, other flags are not. This fixes US 0x153A9 (JNE) and 0xA13A1 (JE) without choosing one predecessor's setter semantics for the entire join.

## Implementation

The fixed-point reaching-definition analysis still rejects an unknown predecessor. When all reaching definitions are CMP or TEST and both kinds occur, it returns MERGED_COMPARE_ZF. In functions with a live equality consumer of that state, every CMP/TEST snapshot also publishes `_zf`: CMP uses masked operand equality, TEST uses masked AND equal to zero. Width differences are safe for this bit because each snapshot masks its own operands before computing it.

The partial state supports equality/inequality only. Carry, signed ordering, sign, overflow, and parity consumers do not get inferred answers from it. Existing same-kind and result-setter merges retain their behavior. Calls or undefined setters continue to invalidate state according to the existing transfer rules. A join whose flags are overwritten before an equality consumer does not cause extra snapshot emission.

This is local to each generated C function. It does not supply flags to independently called fragments. No function address or canonical symbol is hard-coded in the translator for this fix.

## Validation

- Configuration tests run in a fresh process: 5 passed. Remaining translator suite: 201 passed and 41 subtests passed.
- Compiled synthetic backward join tests both JE and JNE, both incoming paths, and all 65,536 low-word input values: 262,144 outcomes. One arm is CMP EAX,1; the other is TEST AH,1. UBSan passes.
- Unknown and unsupported incoming definitions remain unresolved. Non-ZF consumers are rejected. A dead mixed state adds no `_zf` snapshots.
- `validate_mixed_zf.py` extracts actual generated snapshot and branch statements from all ten audited incoming edges: three TEST and five CMP arms into 153A9, plus the CMP and TEST arms into A13A1. Against separately specified x86 ZF outcomes, 9,832,650 predicate checks pass under UBSan. Replacing TEST's AND test with operand equality fails the negative control.
- `mixed_zf_edge_evidence.txt` preserves those exact statements. These tests start at the flag snapshots; they do not execute the preceding FPU operations, the real call at A1397, or complete game functions.

The raw US instruction and canonical provenance underlying these two joins remain recorded in FLAGS_AUDIT.md and the shared-ZF backport documentation. No additional recovered symbols were imported.

## Isolation and remaining work

Generation uses the same US XBE and discovery inputs as the preceding comparison, with zero ABI entries, in `build-macos/jsrf-first-fault/flags-comparison/mixed-zf/{gen,summary}`. The preserved runnable sources and executable are unchanged. Predicate agreement is not a full build/link/runtime or gameplay check.

Independent entry at 14885 still cannot infer incoming flags, and recovered startup/epilogue coverage must be compared before replacing the runtime. Remaining unresolved sites also include discovery/decoding questions and other flag shapes; the audit count is not a count of reachable defects. Completeness of the existing recovered CFG remains an assumption of dataflow analysis.

## Final matched output

After restricting emission to live equality consumers, exactly two function bodies differ from the continuation baseline: sub_00015130 and sub_000A0F10. The only changed conditional expressions are the two target joins. There are no newly unresolved predicates, changes to previously resolved predicates, or unmatched conditional sites. Unresolved conditional markers decrease from 70 to 68. The complete sub_00014870 body is byte-for-byte unchanged.

The final isolated translation reports 10,392 selected functions, 10,503 translated bodies, zero translation failures, and 220 unresolved stubs. `mixed_zf_comparison.json` records the comparison. The final emitted edge checks were rerun after the emission-scope reduction and passed. These are generation and bounded semantic results, not a replacement runtime acceptance.
