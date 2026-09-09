# Current-build traversal verification

The historical GameObj execution/deletion failure did not reproduce in this bounded run. This completes the scoped verification goal, not gameplay acceptance or a proof that all object corruption is impossible.

## Execution

- Built with `cmake --build build-macos/jsrf-first-fault/build -j 4` successfully.
- Binary SHA-256: `6a4f0f5357c06f0cb28a1d1f5c42ff635eb0cff0056db89fabb9964693be744d`.
- Existing generated source and all pre-existing edits preserved.
- Command: `RECOMP_TREE_TRACE=1 RECOMP_STARTUP_TRACE=1 RECOMP_PB_EXEC=1 python3 diagnostics/jsrf_first_fault/run_bounded.py codex-canonical-verify-01 --seconds 180`.
- Runner outcome: `bounded-stop=180.0s`; runner terminates only its own child, with up to five seconds of shutdown grace. The last guest framebuffer report says t=183.02; this is the guest reporter's time, not a claim of an exact 180-second guest interval.
- Logs: `build-macos/jsrf-first-fault/render-investigation/codex-canonical-verify-01/{stdout,stderr}.log`.

## Evidence

- Zero FIRST GUEST FAULT, TREE corrupt-traversal, UPDATE-ABI, REFCOUNT, or ICALL log markers.
- Maximum registered-object count: 138. Last reported count: 134 at startup tick 2736. The historical failure occurred around tick 2522 with 20 objects; this run progresses beyond that approximate stage, although ticks are not guaranteed equivalent workloads.
- Default family state flags remained clear in the inspected progression records.
- 184 framebuffer reports; last report still CHANGED, with 137793/153600 nonzero pixels. This measures guest framebuffer contents, not verified window presentation: the same reporter says presented nonzero=-1.
- Full current diagnostic suite: 21/21 tests passed, including the empty-list repair check and unresolved-flags ratchet.
- Machine-readable marker/progression summary: current_run_summary.json.

## Limits and decision

Existing tree probes observe selected link/traversal points and detect selected anomalies; no report does not prove every link is valid. This build does not contain comprehensive per-block tracing or a complete dump of every deletion dispatch. Since the suspected failure did not reproduce, adding such instrumentation or changing call conventions is not justified by this run. No runtime source changes were made.

There is no new first failure to classify. No controller-driven gameplay, visual correctness, audible output, or global flag-lowering correctness is claimed. The historical AddRef label remains unreliable for the reasons in FOLLOWUP.md.

## Next action

Audit one remaining reachable unresolved-flags branch across a generated fallthrough boundary, using US machine code and a focused reproducer to establish whether live condition flags are lost. This follows the demonstrated historical defect class (G21), rather than continuing to assume an unobserved GameObj-to-AddRef dispatch.
