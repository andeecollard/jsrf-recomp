# Flag-merge audit and bounded backports

## Findings

The initial preserved-source audit found 78 dead conditional fallbacks in 49 functions. This count is a syntactic warning count, not proof every site represents reachable code: some disassembly entries contain obvious data decoded as instructions.

Selected function: US 0x15130, called from the Default execution family. Canonical a4f7af133a15d75ec6a33ff02b3c8b7b64ddffd4 labels it CSysChallengeRegionManager::calledDuringExec0Default. Branch evidence is independently extracted from the US XBE in flags_15130.asm.

### 0x15275

- 0x15271: cmp dword [ebx+0x10],1; falls through to shared JNE.
- 0x152EE: cmp dword [ebx+0x10],2; jumps backward from 0x152F2 to 0x15275.
- Both generated predecessors publish 32-bit snapshots in _fa/_fb.
- Original branch reads constant-zero _flags, so it never takes the branch to 0x153BC.
- Backport now consumes CMP_NE(_fa,_fb), preserving the arriving comparison rather than rereading one fixed immediate.

A fresh single-function translation also emits an unresolved branch. The translator already supports compatible CMP merges, but its address-ordered pass has not processed the later predecessor when it reaches the join. This is an intra-function backward-edge analysis limitation, distinct from the historical cross-function flag-local loss. No general translator fix is claimed.

### 0x153A9

An additional CMP-only repair appeared in the shared workspace during this task. Inspection showed it was incorrect; its tests covered only comparisons and missed the TEST predecessors. That change was corrected in place without discarding other workspace work.

There are EIGHT incoming edges, not seven same-kind comparisons:

- Three TEST AH,1 paths: source blocks 0x1520C, 0x1527B, 0x152BB; tests at 0x1521A, 0x1528C, 0x152C9.
- Five CMP [ebx+0x10],3..7 paths: blocks 0x15311, 0x15333, 0x15356, 0x15375, 0x153A5.

JNE reads NZ. Each TEST edge now writes (_fa & _fb)!=0; each CMP edge writes _fa!=_fb. The join uses the arriving boolean. For example AH=0x40 with mask 1 requires NOT taken, but CMP_NE(0x40,1) incorrectly takes it. AH=1 requires taken, while CMP_NE(1,1) incorrectly does not. The state update at 0x153AB increments object+8 and clears object+0xC/+0x10, so branch direction has concrete guest consequences even though reachability in the bounded title sequence is not established.

## Changes

- backport_challenge_flag_merge.py: narrowly scoped, idempotent 0x15275 repair.
- backport_exec_state_merge.py: corrected mixed CMP/TEST repair with explicit predecessor checks and edge conditions.
- test_challenge_flag_merge.py: executable checks of both comparison paths and boundary values; patch scope/idempotence/rejection checks.
- test_exec_state_flag_merge.py: executable checks of all eight paths, all 256 AH values for each TEST edge, comparison boundary values, patch scope/idempotence/rejection checks.
- CMakeLists.txt: backport presence and semantic checks registered; ratchet now 76.
- Preserved generated recomp_0000.c patched through the scripts; no full regeneration, renderer changes, or guest pointer special cases.

## Validation so far

- Corrected build succeeds; 25/25 diagnostic tests pass.
- Negative control using the prior CMP-only condition fails the mixed-path executable test as expected.
- git diff --check passes.
- Audit: 76 dead fallbacks in 48 functions, down from 78. Adjacent mixed branch now has a real preceding boolean write. The audit's label “legitimate” is only syntactic and is not a proof of all-path correctness; that was checked separately here.
- Before run: codex-flags-audit-01 has no UNRESOLVED-FLAG or FIRST GUEST FAULT markers in the inspected log. The selected branches were not observed executing, so no scene symptom is attributed to these repairs.
- codex-flags-verify-01 used the intermediate shared CMP-only build and was terminated after discovering its defect; it is NOT validation of the final fix.
- Final corrected binary SHA-256: 7e1e6f42b4d10a1b5965db4adf29a3730518078b9ffe9d81036015dcd392f2ab.
- Final bounded run: codex-flags-verify-02, RECOMP_UNRESOLVED_FLAGS=1 RECOMP_PB_EXEC=1, 180-second limit. Result appended below after completion.

## Remaining scope

These are specific backports, not a solution to all 76 remaining fallback sites. General control-flow flag analysis must distinguish backward predecessors, mixed setter semantics, and separate generated-function boundaries. Runtime coverage and a lower syntactic count alone cannot establish correctness of all of them.

## Final bounded-run result

Runner reported bounded-stop=180.0s. Zero FIRST GUEST FAULT, UNRESOLVED-FLAG, REFCOUNT or ICALL markers. The final framebuffer report at guest-reported t=183.05 (including shutdown grace) remains CHANGED, with 113808/153600 nonzero pixels. Window presentation was not established by that report (presented nonzero=-1). No new first failure appeared, but this run does not prove that either repaired site executed or that gameplay is correct.

Next action: add an order-independent flag-state analysis for backward CFG predecessors, with a small regression based on the two incoming comparisons at 0x15275; retain a conservative result for mixed CMP/TEST joins until their individual flag semantics are explicitly represented. Do not treat the 76-site audit count as 76 identical defects.
