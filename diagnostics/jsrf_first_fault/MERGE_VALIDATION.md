# JSRF upstream merge validation — 2026-09-03

The interrupted merge was resumed on `jsrf-vblank-handshake` without resetting,
aborting, stashing, or replacing either parent's files wholesale.

- Local parent / rollback tag `pre-workbranch-merge`:
  `1bde663e566d4bc536c8ec9ff0ff42239c8a0fed`.
- Upstream parent `origin/work/v0.7.0-non-local`:
  `342eb1fa3a31c64a012e7d7ae1ea3a69f6ca3962`
  ("Map flash, open the window from the executor, and arm the watchdog").
- The original merge had 20 conflicted files / 39 hunks. At handover,
  4 files / 20 hunks remained unresolved; those were completed. Previously
  staged resolutions were revisited where tests or builds exposed a conflict.
- Final conflict-marker scan and Git whitespace checks pass.

## Semantic resolutions

- **Padded INT3 termination remains.** The existing test failed when it was
  dropped. It now fits upstream's return/jump and forward-target logic, while
  retaining embedded switch-table handling. An unpadded debug trap does not
  terminate a function, and a reachable forward target still extends it.
- **XADD / LOCK XADD and CMPXCHG remain.** Width handling, atomic memory
  updates, old-value return, and flags are retained. The 15 existing XADD and
  CMPXCHG tests pass, including compiled atomic-helper and ADD-flag checks.
- **Upstream MMX and bit-scan implementations supersede the local duplicates.**
  MMX uses the upstream register union and helpers. Bit scans publish ZF through
  upstream's operand snapshots. Tests now check that representation without
  dropping zero-input, width, or flag requirements.
- **FS remains per-thread.** Accessors use `g_fs_base`, consume the renamed
  `mem_seg` field, and add the segment base once. The primary TIB follows
  upstream's relocated address; worker TIB/TLS setup is preserved.
- **AV uses upstream's unshifted standard IDs plus `AV_STANDARD_SHIFT`.**
  Guest option numbering remains compatible with JSRF: numeric option 6 returns
  the packed capability word `0x00480104`, including the dashboard's 480p flag.
  The regression test checks this literal ABI value independently of constants.
- **Guest kernel objects retain 32-bit layouts and token validation.**
  IoCreateDevice's extension lifetime handling, thread objects, Partition0/5
  device behavior, completion handling, AC97 readiness, AArch64 MMIO traps,
  vblank acknowledgement, and macOS mappings are retained alongside upstream
  additions. Duplicate wrappers, switch cases, and global definitions were removed.
- **Partition1 remains writable save storage.** Mapping it to the game folder
  caused an early firmware exit; restoring its local data-volume mapping fixes it.
- **Discovery preserves upstream aliases with two admission fixes.** Alias
  extents cannot suppress independent vtable seeds. Immediate references use the
  existing instruction-covering/prologue guard: `0x001600CA` had incorrectly
  become a `leave` function inside the `test ecx, ecx` at `0x001600C9`, truncating
  QueryInterface and corrupting the stack. Focused regression tests cover both
  failures. The proven `0x0007BE30` SEH seed is recovered.
- **POSIX contiguous allocation retains local guest-RAM backing.** Upstream's
  allocator interface and Windows arena remain. On POSIX, allocating a framebuffer
  in separate high-address storage made the GPU's low physical-offset clear
  overwrite guest code; the raster test reproduced this fault. Unpinned POSIX
  buffers therefore continue using the guest heap. Fixed-address pinned buffers
  retain their existing separate window.
- **Build interfaces are reconciled.** Debug-service output is separate from
  overridable trace hooks; Windows-only FMV access is guarded; missing portable
  declarations are supplied. The local framebuffer stub cannot mask the upstream
  Windows presenter. Regeneration synchronizes the generated runtime header.

## Validation

Used the existing diagnostic build directory and target:

```sh
cmake --build build-macos/jsrf-first-fault/build \
  --target jsrf_first_fault jsrf_av_encoder_option_test --parallel 6
ctest --test-dir build-macos/jsrf-first-fault/build --output-on-failure
```

- Build: pass. Existing compiler/linker warnings remain; this is not a
  warning-free build.
- AV CTest: 1/1 pass.
- Python regression suite: 221 tests and 13 subtests pass across `tools/recomp`,
  `tools/disasm`, `tools/func_id`, and `tools/kernel_audit`, excluding
  `tools/recomp/test_config.py` from the shared process.
- Configuration self-check: all 5 pass in a fresh process. Its initial-global-
  state assertion is order-dependent when other tests have configured the module.
- Global callback discovery self-check: all 4 cases pass.
- Python compile check: pass.
- JSRF regeneration: converged in 3 rounds; 10,391 translated functions,
  1 manual function, 0 translation failures. Generated output remains ignored.
- Temporary ABI instrumentation and the generated-code/dispatch comparisons
  were removed. `CMAKE_C_FLAGS` is empty and `RECOMP_GEN_DIR` again names
  `build-macos/jsrf-first-fault/gen`.

## Runtime comparison and limits

The preserved pre-merge executable was rerun with desktop access and reproduced
the recorded graphics counters. The merged executable was run from the same
directory with normal settings for 20 seconds, followed by the existing raster
self-test for 10 seconds. Each child was stopped by the bounded runner; neither
final run exited early or reported a guest fault. POSIX audio output remains
silent, as before, while the ADX worker tick continues advancing.

| Observation | Pre-merge | Merged |
| --- | ---: | ---: |
| Command words | 3,945 | 3,151 |
| Methods | 2,734 | 2,172 |
| Unhandled methods | 1,328 | 1,060 |
| Bad headers | 0 | 0 |
| Clears / flips | 9 / 4 | 7 / 3 |
| Real geometry draws | 0 | 0 |
| Main loop `sub_00013F80` | Reached | Reached |
| ADX tick | Advancing | Advancing |
| Raster self-test triangle | Visible | Visible |

Read-only stack samples show both versions executing the same main-loop sites:
`sub_00013F80 + 740` through `sub_00145CA6` / `sub_00145C28` /
`KeDelayExecutionThread`, and `sub_00013F80 + 592` through the update path.
The known runtime frontier is therefore reached; this is not a claim of identical
startup output. The reduced startup counters also occur with the preserved old
generated code linked to the merged runtime. Omitting only the repaired manual
dispatch entry did not change them. Their precise remaining cause is unassigned.

The raster test now writes a valid 640×480 magenta triangle, with no guest fault
or off-surface triangles. Neither version reaches the real game-geometry render
chain. That existing blocker was not investigated further. Windows and native
32-bit x86 conformance were not exercised on this macOS/AArch64 host. The POSIX
high-window GPU address-translation limitation remains explicit in the allocator.

Local evidence is under ignored `build-macos/merge-audit/`: `build-final.log`,
`ctest-final.log`, `tests-5.log`, `config-test-final.log`,
`callback-test-final.log`, `regenerate-3.log`, `baseline-desktop.stderr.log`,
`merged-final.stderr.log`, the two `*-sample.txt` profiles,
`merged-raster-final.stderr.log`, and `verified-raster-002.png`.
