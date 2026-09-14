# Gameplay profile: part of “rest” is untimed GPU synchronization

14 September 2026. Source HEAD cbb2700, current generated tree
f0d5dca6a9a4a3b9, title -O2, RECOMP_METAL_BATCH=0. No runtime changes.

## Capture

Run: `build-macos/jsrf-first-fault/measure/codex-gameplay-profile-20260914/`.
The directory contains stderr.log, matching source/copy HDD manifests, and
three 12-second macOS sample captures. The HDD manifest hash prefix is
94faf86c3be755d4. The initial attempt against build-O2 was rejected as stale;
the actual run used build/jsrf_first_fault. Startup confirms the writable MCPX
alias is active.

The existing measure.sh ran with pad/gameplay.pad and a 300-second bound.
Profiles began at approximately 76, 120 and 167 seconds after process launch.
The first capture was in the tutorial, the other two after scripted movement
began at input time 105 seconds. The log progressed beyond the title plateau
through live=58–77; live is an object count, not a unique scene identifier.
Voice retirement also increased (off=84 then 95 in later reports). These
captures are not the old attract-screen profile. They do not constitute a
visually matched Dogenzaka Hill benchmark.

## Findings

The frame timer runs at FLIP_STALL on the pushbuffer thread. Its `rest` is
wall time minus three timed regions, not measured guest CPU time.

In moving.sample.txt, rendering thread 768554 has 7,983 samples in its main
stack tree. Of these, 1,021 are under one clear_surface call path (12.8%).
940 descend through nv2a_metal_invalidate, including 574 waiting for a Metal
command buffer and 265 in the GPU readback call. These are inclusive stack
counts: do not add parents to children.

The later capture repeats the finding: 1,150 of 7,955 rendering-thread
samples (14.5%) follow that same clear path, with 1,050 in invalidate.
Additional clear paths are small. This is repeat sampling within one run,
not independent-run replication.

Source inspection explains why these costs appear in `rest`: clear_surface
calls nv2a_gpu_invalidate_range without a stage timer. On Metal that macro
calls nv2a_metal_invalidate. The reported `sync` timer surrounds snapshot
synchronization, not every GPU synchronization call. Thus “sync=1 ms” does
not mean all GPU synchronization costs 1 ms.

The moving capture also contains 214 samples in the software-method yield
path, and the later capture 233. Other residual candidates include untimed
batch preparation, dispatch, presentation and gaps between submissions.

The first capture's main guest thread has 4,166 of 7,871 samples in one
KeWaitForSingleObject branch, while a separate guest worker has all 7,871
samples in sub_0013B180. Neither count can be added to the renderer's wall
budget: these threads overlap. A process-wide sum of sleeping-thread samples
cannot establish that the frame is mostly waiting.

## Limits and next measurement

The profile identifies a concrete, substantial contributor to `rest`; it does
not partition the full residual into milliseconds or establish a speedup.
Window timings vary with the moving workload. Sampling adds overhead, and
the current binary unconditionally logs and flushes every OHCI interrupt-mask
write; the source comment claiming only a handful per run is obsolete.
Do not compare this run's FPS directly with the earlier 31.3 FPS figure.

The next targeted instrument should time clear_surface as a separate,
non-overlapping stage, then separate software-method waits and presentation.
Keep GPU synchronization nested inside clear/submit attributed to those
stages rather than adding it again. No synchronization or guest wait should
be removed on the strength of this profile alone.

## Run completion

The run completed the full 300-second bound and was stopped by the harness
with SIGTERM. No FIRST GUEST FAULT was logged. The final reported live count
was 74. There were 70608 unconditional interrupt-mask write log lines,
reinforcing that logging overhead must be controlled in a timing comparison.
