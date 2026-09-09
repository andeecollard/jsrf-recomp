# RESULT

The walker is confirmed CActBase/GameObj execution and deferred deletion (answer A), and both suspect pointers were legitimate members of that hierarchy in pre-corruption captures. The premise that this walker dispatched to AddRef is NOT PROVEN. The old fault heading records the last instrumented function, which may already have returned. The active G20 handoff reports the historical corruption fixed by restoring an empty-list branch; that fix is present and its check passes. No new runtime fix is justified.

# GAMEOBJ MAPPING

Canonical revision remains a4f7af133a15d75ec6a33ff02b3c8b7b64ddffd4; see REPORT.md for release/digest limitations and verified function mappings.

- CActBase::recursiveExec1Default -> 00011D00 -> CONFIRMED.
- CActBase::deleteActionChild -> 00011B90 -> CONFIRMED.
- Captured object 042D0E60 -> vtable 001C4D10, canonical UnknownObj_0x6 -> STRONG class identity, CONFIRMED captured vtable correspondence. Constructor 00024670 writes that vtable after base construction.
- Captured object 041E3010 -> vtable 001CB0A8, canonical CProgress -> STRONG class identity, CONFIRMED captured vtable correspondence. Constructor 00066870 writes that vtable after base construction.

The class labels remain recovered names, not proof of original source nomenclature. Neither object's sampled Exec1 slot points to AddRef: both are 00011C90. Raw US slots for UnknownObj_0x6 are destructor 000246E0, Exec0 00024700, Exec1 00011C90, draw 00024400; CProgress has 00066900, 00066440, 00011C90, 00066550.

# DRAW WALKER STRUCTURE

The former DRAW label means Exec1/deletion here. Values below come from claude-apccheck-01 STARTUP-OBJECT captures (lines 787 and 850); they describe capture time, not guaranteed fault-time contents.

| Offset | Meaning | 042D0E60 | 041E3010 |
|---|---|---|---|
| +00 | vtable | 001C4D10 | 001CB0A8 |
| +04 | flags | 00010003 | 00010003 |
| +08 | action ID | 00000006 | 00001DEF |
| +0C | draw-child mask | 00000007 | 00000007 |
| +24 | parent | 040FFFA0 | 040FFFA0 |
| +28 | child | 0 | 0 |
| +2C | previous sibling | 0 | 042D0F40 |
| +30 | next sibling | 042D0F40 | 0 |
| +34 | draw-next | 042D0F40 | 0 |
| +38 | draw back-link | 040DBA0C | 042D0F74 |
| +3C | draw last-link pointer | 040DBA10 | 040DBA10 |
| +40 | sorted draw link | 0 | 0 |

All listed layout roles are established by the earlier structural map. Exact allocation sizes, reference-counted members and fault-time object contents remain UNKNOWN. Snapshot lengths and distances between allocations are not object-size evidence.

Later codex-sep05-tree-01 records CProgress flags 00010007 and next sibling 042DF870: ordinary changing state must not be mistaken for inconsistency. Its final history changes from a valid node traversal to node 81688000, whose interpreted child is 10A410A3. That demonstrates traversal corruption before the fault; it does not identify the writer. The probe's method field outside pc 11D63 reads slot zero, so those fields must not be interpreted as Exec0 targets.

# sub_00177FE0

CONFIRMED stdcall atomic reference-count increment: load object from [esp+4], lock xadd at object+8, return incremented count, ret 4. Current generated instructions agree despite the generated metadata comment saying cdecl.

Canonical symbol table calls it AddRef and associates it with Controller. Raw US Controller vtable 001E3828 begins QueryInterface 00167CD0, AddRef 00177FE0, Release 00166CA0. Thus AddRef is at **vtable+4**, while the count is at **object+8**. The historical claim that it is always at vtable+8 is disproven by this concrete table.

Known legitimate call: QueryInterface at 00167D41 retrieves the object argument, stores it to the output pointer, loads its vtable, pushes the object at 00167D4D, and calls [vtable+4] at 00167D4E. This establishes a COM-style interface contract. A thiscall Exec1 call without that push would be incompatible, but no captured call establishes that it occurred. Membership in Controller's table does not imply the helper is exclusive to Controller or identify a particular member of the failing GameObj.

# ROOT CAUSE

The exact alleged GameObj-to-AddRef dispatch is NOT YET PROVEN and may be an attribution error:

1. recomp_trace_enter sets g_current_guest_function; recomp_trace_exit does not restore a caller. The old heading TRANSLATED GUEST FUNCTION therefore is not a reliable current-PC symbol. Current main.c correctly labels it LAST INSTRUMENTED GUEST FUNCTION (may have returned).
2. The historical capture has no guest block history. Its host fault address 3EF8F8F7A is guest-base plus ESI EF8F8F52 plus 0x28, consistent with a bad child-link read; AddRef accesses object+8 and overwrites ECX. This is supporting evidence, not host-PC symbolication against the historical executable.
3. Stack return 00011D42 belongs to descendant destruction; normal Exec1 return would be 00011D6A.

The independently recorded G20 corruption mechanism is lost condition flags across generated fallthrough functions at 00014885. It misses an empty-list guard and reaches unsigned count-1 copying in 000147A0 with count zero. Existing guarded-page measurements identified that writer overwriting the kernel thunk table. This mechanism is a documented historical cause, but those measurements alone do not reconstruct every write or the exact final instruction in the earlier apccheck crash.

# FIX

NONE in this follow-up. The existing backport_empty_list_guard.py repair is already applied in generated sub_00014885. No symbols imported, no renderer changes, no guards or object skipping added. Existing changes preserved.

# VALIDATION

- Read raw US bytes for AddRef, Controller QueryInterface/Release, both class constructors, list guard and copying loop; saved in followup_disassembly.txt.
- Re-read pre-corruption objects and TREE history from claude-apccheck-01 and codex-sep05-tree-01.
- Re-read active G20 closure and inspected claude-guardfix-12: no FIRST GUEST FAULT or REFCOUNT match in that log; it continues to ADX tick 6481. The handoff reports two successful bounded runs and increased object/draw counts. This is existing-run evidence, not a new before/after experiment.
- Ran ctest --test-dir build-macos/jsrf-first-fault/build -R 'empty_list_guard|unresolved_flags' --output-on-failure: 2/2 passed. These are presence/audit checks, not proof all flag-lowering defects are repaired.
- No new game run or graphics comparison; no runtime changes to validate in this follow-up.
- Attempted both user-linked YouTube pages and exact-ID search. Retrieval failed and searches returned no results. No claims were derived from unseen video contents; canonical source/machine code supplied the findings.

# NEXT STEP

One bounded current-build reproduction with guest-block history and a rolling record of the actual Exec1/deletion indirect call site, node, vtable slot, target, registers and stack, dumped on the first fault. This would establish whether any current failure remains and its actual dispatch, without assuming the old AddRef label. Under the requested stop condition, specify this discriminating probe rather than adding speculative code.
