# JSRF pushbuffer address-contract investigation

Audience: repository maintainers. Date: 5 September 2026. Scope: continue G26 from the saved ordering test; no renderer redesign or upstream merge.

## Measured result

The macOS contiguous allocator returned low CPU addresses even after the harness enabled the shared high physical alias. JSRF reconstructs DMA_GET with bit 31 set, so its reserve routine compared addresses from different ranges. Correcting that contract removed the rejected jump in one 110-second run. The run later stopped submitting in a different D3D event wait; the overall graphics problem remains open.

## Evidence before the change

The fresh 100-second baseline reproduced a rejected jump at 005E8960. Its dword remained 3ECDCB99 after the barrier and both delays. The reserve probe at 001915FD recorded ring 0056D000-005ED000, effective GET 80000000, and raw GET 0057326C. Generated code at 00191598 supplies the fallback GET when the high reconstructed address fails the low ring bounds check. These observations establish the address mismatch. Overwriting unread commands is the resulting causal explanation, not a captured store-by-store trace.

## Change and verification

When the physical heap alias is enabled, xbox_ContiguousAlloc now returns the high CPU address of the same low GPU backing. Without the alias, the existing low allocation contract remains. The harness normalizes ring bounds to physical offsets. Tests cover allocation address round trips and shared bytes. All 17 CTest checks passed. The corrected run consumed 4,743,913 dwords with zero bad headers and no rejected jump, invalid header, CALL fault, or latched stream fault reported. The permanent stream_fault behavior is unchanged.

## Remaining limit and next investigation

Submission stopped with index PUT and GET both 14991. A one-second stack sample found all 387 main-thread samples in sub_00191440, at its event wait at 00191510, reached through sub_00191530. This is not the earlier 001914F0 fence polling loop. The next measurement should capture the event at device+2440, the reserve distance calculation and the NOTIFY/NO_OPERATION dispatch that should signal it. No completed frame, sustained rendering, or playable result is claimed. One bounded run is insufficient to close G26.

## Evidence

- [Baseline trace](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/render-investigation/codex-reserve-baseline/stderr.log)
- [Corrected trace](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/render-investigation/codex-reserve-fixed/stderr.log)
- [Stack sample](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/render-investigation/codex-reserve-fixed/stack-sample.txt)
- [CTest result](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/render-investigation/codex-reserve-fixed/ctest.log)
- [Guest reserve implementation](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/gen/recomp_0009.c)
- [Allocator change](/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/src/kernel/xbox_memory_layout.c)
