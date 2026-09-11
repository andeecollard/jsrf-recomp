# Windows AC97 oracle: reset fixed, next boundary captured

Date: 2026-09-11 (Europe/London)

## Result

The AC97 bus-master write-clear implementation was compiled but unrun in the
handover. Its first execution failed exactly at the warned-about interaction:
`xbox_McpxTrapInstall()` guarded the AC97 page, then the runtime's initial
codec-ready store to `0xFEC00130` faulted. The harness VEH routed only the APU
and NV2A address ranges, so it never offered that store to
`xbox_Nv2aHandleWin32Fault()`.

`diagnostics/jsrf_first_fault/main.c` now routes the guarded AC97 page
(`0xFEC00000..0xFED00000`) through the same decoder as the guarded PCRTC page.
This also covers later codec-ready stores made by the runtime's acknowledgement
thread.

The next Windows run passed every cheap success check from the handover:

- both guarded-page startup lines appeared;
- interrupt vectors 3, 1, 6, and then 5 connected (vector 5 routine
  `0x001A2681`);
- `[APU] started by the title` appeared;
- ADX advanced from tick 0 past tick 2,000;
- file opens continued beyond the previous count of four;
- vblank reported 1,234 deliveries in 19,907 ms (62.0 Hz), with zero
  unacknowledged skips;
- no guest fault occurred before the diagnostic run was stopped.

The run used the existing `recomp-gate` CrossOver bottle, `build-fh`,
`gen-fh`, and a fresh clone of the disposable Windows HDD. Its log is
`scratchpad/codex-ac97-win-2.log` in Claude session
`b79c30e6-db49-4a18-b699-7f315630081b` under `/private/tmp/claude-501/`.

## Next differential boundary

A matching macOS run used the same `gen-fh`. Windows thread 0 stopped growing
at 10,840 instrumented entries while macOS continued. To obtain overlap, the
per-thread circular ring was temporarily enlarged to 131,072 entries for one
macOS capture and then restored to 1,024.

After collapsing consecutive repeats, Windows' final 100 entries occur
verbatim in the macOS trace at collapsed indices 13,098 through 13,197. Both
end that common suffix at:

```
... 00191440 00191390 001912A0 00191150 001910E0
```

macOS then continues with:

```
00192820 00155B20 001900D0 00155B40 00190490 ...
```

`sub_001910E0` is a short leaf D3D helper that computes a distance from device
and push-buffer fields and returns; it contains no wait. `sub_00192820` reads a
resource data offset and ORs in `0x80000000`. Therefore the evidence only
places the Windows stop after entry to `sub_001910E0` and before the next
instrumented entry. It does not establish that either helper is the cause.
The next useful instrument is a return/caller or block-level trace around this
boundary, so the uninstrumented caller is named without widening to all game
functions.

## Validation notes

- The `jsrf_first_fault` target builds on both MinGW and macOS after restoring
  the normal 1,024-entry ring.
- The instrumented `gen-fh` CTest run passes 21/27. The six failures are anchor
  and ratchet checks that assume a different generated-tree state; this is the
  as-found, already instrumented gen called out in the handover.
- A MinGW all-target build still reaches unrelated pre-existing test-target
  portability failures (`setenv` in `jsrf_pb_scan_owner_test` and missing
  `nv2a_vsh.h` includes in D3D8 smoke targets). The Windows oracle target
  itself builds cleanly.
