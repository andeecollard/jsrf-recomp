# Bounded continuation flag preservation

The translator now preserves the CMP at US 0x1487E through the JBE at 0x14885 on entry through sub_00014870. It duplicates a bounded, proven straight-line continuation into the flag-producing function, retaining independently addressable entries. No runtime-global flag state or address-specific translator exception is introduced.

The existing canonical GameObj/traversal mapping and executable provenance in REPORT.md remain the basis for this investigation. This change addresses generated-code semantics; it does not import additional recovered symbols.

## Eligibility

The starting fragment must decode exactly, contain no branch/call/return, and end with known CMP/TEST state. Adjacent function entries may be followed only through flag-transparent straight-line instructions up to the first conditional consumer. Expansion is limited to eight entries and 16 KiB. Manual overrides, owned entries, recovered CFG targets, incomplete decoding, calls, and intervening flag setters stop expansion. Original function database entries are not changed.

In the fresh JSRF database, the initial 0x14870 body already includes the instruction at 0x14881 and ends at 0x14885. Therefore this run inlines only the 0x14885 entry, extending the body through 0x14955. The synthetic regression separately exercises three adjacent fragments, matching the historical three-function split.

## Evidence

The isolated output is build-macos/jsrf-first-fault/flags-comparison/continuation/{gen,summary}, using the same US executable and discovery inputs as FRESH_TRANSLATION.md. Translation reports 10,392 selected functions, 10,503 translated bodies, zero failures, and 220 unresolved stubs. These are generation results, not a full runtime build.

Compared with the preceding fixed-point CFG output in flags-comparison/current, only the generated sub_00014870 body changes; no function bodies are added or removed. Its first branch now reads CMP_BE(_fa, _fb) and jumps directly to 0x14909 for an empty first list. There are no unresolved flag markers in that body.

The total unresolved marker count stays at 70 because the original standalone 0x14885 entry remains available and cannot infer incoming flags. This fix proves the path entered at 0x14870, not arbitrary direct entry into a fragment. Other continuation shapes, including branches before the boundary, remain outside scope.

## Validation

- Broad translator tests: 5 configuration tests in a fresh process plus 195 remaining tests and 41 subtests passed. Two additional exclusion tests subsequently passed with the six-test continuation suite.
- Synthetic generated C executes six unsigned input values across three fragments, including zero and both signed extremes. Disabling inlining makes that regression fail as expected.
- validate_continuation.py extracts and compiles the actual newly generated US sub_00014870. Empty and one-element first-list cases pass under ASan and UBSan; it checks ring/data changes, stack balance and preserved EBX/ESI/EDI. ECX returns the scratch loop count, as dictated by the original stack slot reuse. The second list is empty in both fixtures, and callbacks are configured to abort if unexpectedly reached.
- The runnable JSRF generated sources and executable were not replaced or rebuilt.

The existing historical guard backport remains necessary in the preserved runtime. Before any full regeneration replaces it, broader callback/list cases and the other retained backports must be verified in the replacement output. This result alone does not establish gameplay correctness or resolve the remaining mixed CMP/TEST merges.

## Expanded verification and replacement audit

The follow-up goal completed the bounded walker matrix and audited retained repairs. `validate_continuation.py` now also compiles `continuation_matrix.c` with the actual generated function inserted. Under ASan/UBSan, 12,960 fixtures pass against a separately written ring/list model: capacities 1–5, both list counts 0–8, and 32 index/data seeds. The comparison covers guest memory below the stack region, ordered callback arguments, expected callback return addresses, final stack position, and preserved EBX/ESI/EDI. Across fixtures it exercises 21,420 first-list wraps, 23,724 second-list wraps, 10,818 first-list callbacks, and 23,328 second-list callbacks. Forced-taken and forced-not-taken versions of the initial guard both fail the matrix, as intended.

Callbacks are test substitutes that remove/compact an entry, clean up the guest argument/return slots, and clobber EAX/ECX/EDX. They are **not execution of the real callees**. The test validates walker behavior for that mutation contract; it does not establish complete callee semantics, arbitrary callback mutations, or valid capacities/counts outside the fixture domain. The stack region is excluded from bulk memory comparison because generated PUSH operations intentionally write it; final stack/register checks remain explicit.

`audit_fresh_backports.py` records source excerpts in `fresh_backport_audit.json`. It searches function identities rather than assuming unchanged chunk numbering. This is a source-shape audit, not full semantic equivalence:

| Retained repair | Fresh output |
|---|---|
| Empty-list guard entered through 14870 | Present in the inlined body |
| Challenge join 15275 | CMP_NE snapshot present |
| Execution-state join 153A9 | **Missing: unresolved mixed CMP/TEST NZ** |
| Shared join A13A1 | **Missing: unresolved mixed CMP/TEST ZF** |
| Loader join 13D3F3 | CMP_LE snapshot present |
| Result joins 1404DB / 1404F1 | EAX/EDI result conditions present |
| ADX 145181 block | DEC snapshot and snapshot-based JNE present |
| FCMOV 14C850 / 14C870 | TEST-based conditional moves present |
| Independent entry at 14885 | Still unresolved; no borrowed incoming flags |

The next implementation priority is conservative per-edge ZF/NZ handling for mixed CMP/TEST joins, with regressions for **both** 153A9 and A13A1. Unknown incoming paths must remain unresolved. Independent fragment entries and preservation of recovered startup/epilogue entries also need review before any replacement build. The preserved runtime has not been changed by this follow-up.

Follow-up: [MIXED_ZF.md](MIXED_ZF.md) records the completed conservative ZF implementation for both previously missing mixed joins. In its isolated output, those two predicate gaps are resolved; the independent-fragment and recovered-entry review remains outstanding before runtime replacement.
