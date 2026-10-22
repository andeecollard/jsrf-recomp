# Discovery, lifter and APU integration — 22 October 2026

Base: `935117f` (`accuracy-gaps-and-gameplay-harness`, including the nine
local commits ahead of the remote). Upstream reference: `f0f8a46`, v0.16.0.
Work is isolated on `codex/jsrf-discovery-lifter-apu` in the scratch clone
under `upstream_xboxrecomp_clean/tmp/upstream-fixes`. The preserved checkout
and existing integration worktree have not been edited.

## Imported behavior

Discovery incorporates upstream's current `tools/disasm` changes through
v0.16.0, including the guarded virtual-call tails / escaping-method proofs
from #236, byte-index-table exclusion, bounded aliases, code-section pointer
tables and functions after NOP padding. Local declared function bounds,
seed provenance, switch-arm demotion and alias-switch extent recovery remain.
The decoder retains JSRF's monitored-store address metadata while adding the
instruction groups and register-write facts needed by the new entry proofs.
Both upstream and local regression fixtures run together.

Lifter imports:

- `880cda2`: FRNDINT follows the guest x87 control word. FIST/FISTP use the
  same rounding helper and destination-width integer-indefinite behavior,
  retaining the local monitored-store wrappers.
- `114193f`: 16-bit push/pop changes ESP by two bytes. The integration also
  samples a word push's source before decrementing ESP, so `push sp` and
  `push word [esp]` retain the hardware's source address/value.
- `facb626`: MOVSX sign-extends BP and SP.
- `54b64ca`: POP32 writes its destination after advancing the stack pointer,
  so POP ESP leaves the popped value unchanged.

Existing local CFG flag-flow, LOCK XADD and FCMOV handling remain; this is
not a wholesale replacement of the JSRF lifter or all upstream FP/SSE work.

APU imports #217 (`b380f02`, `8627159`, `70afb54`, `3ad36ca`) with its
`aabebed` follow-up: ADPCM buffers cover all 32 descriptor widths, voice locks
wait only for that voice, gains ramp across the frame, HRTF defaults on with
`RECOMP_APU_HRTF=0` available. Existing JSRF trap-held voice traversal and MMIO
handling remain. Voice and ring diagnostics from #195 retain their follow-up
switch parsing and lifecycle cleanup (`b7c311a`). The upstream WAV writer
replaces the old local tap; `RECOMP_APU_WAV` remains an alias, and
`RECOMP_AUDIO_WAV_SECS` bounds captures. The existing SDL output, priming,
pacing and host starvation counters remain; the upstream host-device API
and scheduler have not been substituted.

## Reproduction and validation

Use Python with Capstone and pytest. This session uses a temporary environment
at `/tmp/jsrf-upstream-tools`; it is not an installed project dependency.

```
python -m pytest tools/disasm tools/recomp -q
cmake -S tests/apu_vp -B build-validation/apu-vp
cmake --build build-validation/apu-vp --target apu_vp_test
ctest --test-dir build-validation/apu-vp --output-on-failure
cmake -S tests/apu_wav -B build-validation/apu-wav
cmake --build build-validation/apu-wav
ctest --test-dir build-validation/apu-wav --output-on-failure
cmake -S tests/stack_ops -B build-validation/stack-ops
cmake --build build-validation/stack-ops
ctest --test-dir build-validation/stack-ops --output-on-failure
```

The voice test is adapted to the existing portable thread API on macOS and
covers all ADPCM widths, per-voice locking, volume ramps, disabled diagnostics,
positive voice-sample observations, stale ring laps and shutdown cleanup.

Regeneration now accepts `XBE`, `OUT` and `ICALL_DB` overrides, creates its
output directory, writes the XBE analysis JSON itself, passes the XBE path to
mid-function recovery and splits generated C in groups of 250 functions.
It retains the existing fixed-point discovery and mandatory recovery steps.

The original observed-target JSON is cloud-only and could not be read.
Validation uses a scratch database containing 537 targets merged from the
preserved `/Users/andrewcollard/jsrf-build/runs/bblive/icall.dump`.
This is not a replacement for the original database and no baseline file was
modified. Generated files and build output remain untracked.

Final measurements are appended below after the fresh regeneration/build.

### Completed source-level checks

- Python discovery/lifter suite: **473 passed, 308 subtests passed, no skips**.
- Portable voice-processor CTest: **1/1 passed** (includes diagnostics controls).
- WAV CTests: **8/8 passed**, including capped capture, exit, hard-exit header
  recovery and watchdog close.
- Guest-stack CTest: **1/1 passed**.
- All runtime libraries build on macOS ARM64. Existing warnings remain;
  this is not a warning-free build.
- `git diff --check` passes.

These checks establish the imported operations and runtime build, not that the
intermittent DirectSound guest crash is fixed. That requires repeated gameplay
runs with the handler/voice-lifetime evidence; a single clean boot is insufficient.

### Fresh regeneration, executable and bounded run

Discovery converged in four rounds (new thunk seeds: 721, 44, 9, 0).
The final map contains 9,315 functions: 9,313 translated, two manual,
zero translation failures. Mandatory recovery emitted four additional
mid-function entries. The generated header/tools hashes match CMake's checks.

The full fresh executable and all diagnostic targets build. **28/28 registered
JSRF CTests pass.** Older STATUS notes mention two failing site-specific tests;
this base already retired those two in CMake on 14 September in favor of the
fixed lifter and its semantic tests. This integration did not disable them.

A run with `RECOMP_PB_EXEC=1`, `RECOMP_METAL=1`, the existing gameplay pad
schedule, OHCI attachment and a disposable APFS copy of the HDD ran for
180.03 seconds and was stopped by the bound (SIGTERM, return code -15).
It did not end in a recorded guest or host crash. Last periodic reports:

- 410,204 native Metal batches, zero software fallbacks.
- 5,633 flips; the 5,400-frame presentation marker was reached.
- 242,155,661 pushbuffer dwords consumed, zero malformed headers.
- 19,255 USB TDs retired, zero transfer errors; 101 scheduled pad events fired.
  Events alone are not proof of gameplay input; the continuing USB completion
  counts are the independent servicing evidence.
- Guest PCM production approximately 47,850 frames/s in the last report.
- The capped WAV is 2,880,000 stereo frames at 48 kHz (60 seconds), with
  3,521,277 nonzero component samples and peak magnitude 13,952.

**Limits:** CoreAudio refused device startup with AudioQueueStart error -66681,
so no audible-output acceptance is claimed. The runtime still reported one
unresolved indirect target, `0x0017D15C`, during boot; there is no claim of zero
unresolved calls. The available older baseline function map also lacks an entry
at that address, but that does not by itself establish baseline runtime behavior.
No performance improvement, full gameplay acceptance or elimination of the
intermittent DirectSound fault is claimed from this one run.

Evidence is under `build-macos/upstream-fixes/validation/` and
`build-macos/upstream-fixes/runtime/` in the scratch clone. `runtime/result.json`
records the bound and exit code, `stderr.log` contains the actual counters and
failure lines, and `output.wav` is the capped PCM capture. Build/generated files
and game data are not committed.
