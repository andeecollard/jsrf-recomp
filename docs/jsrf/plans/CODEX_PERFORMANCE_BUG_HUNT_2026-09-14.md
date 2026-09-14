# Performance bug hunt, 14 September 2026

Read-only runtime review; no new gameplay run or performance A/B was performed.
Runtime files were being updated during review. The deferred-vblank retry and
explicit ISR-entry result are now present; the older review findings should
not be carried forward as if those changes were absent.

## Evidence and limits

`render-investigation/irq-new-3/stderr.log` ends with windows around 42 ms/frame:
8.2 ms vertex preparation, 9.0 ms submission, 3.3 ms sync, 22 ms residual.
Residual is not guest CPU time: it includes waiting and untimed runtime work.

`render-investigation/bimodal-1/stderr.log`, around t=36 s, reports 572 batches
per frame, 119.14 ms vertex preparation, 80.07 ms submission, 5.17 ms sync,
53.41 ms residual. USB TD retirements and WDH clears are progressing at this
point. This is an attract-screen observation, not a gameplay benchmark.
The log starts with translator=deadbeefdeadbeef and a STALE warning. This
appears to be the manifest negative control; establish provenance before
using this run as a clean baseline. It does not prove the translated code is
actually stale, but the current stamp cannot establish that it is current.

Stage timers surround entire calls and measure elapsed time, including host
descheduling. Their window totals reset at report time while frame intervals
are recorded at flips, so a short window can straddle partial frames. Prefer
several stable windows and a profile of the rendering thread.

## Main performance targets

1. **CPU vertex interpretation, repeated per index occurrence.**
   `src/kernel/nv2a_pb_exec.c:prepare_vertices` traverses `idx_count`, fetches
   attributes and calls `nv2a_vsh_execute` each time. There is no within-batch
   transformed-vertex cache. Measure indices versus unique indices first;
   cache complete transformed outputs for repeated indices in eligible
   indexed draws, with batch-local lifetime. Validate identical outputs,
   including colours and texture coordinates, and preserve inline-vertex
   semantics. Larger follow-up: specialise decoded programs or translate them
   to Metal; do not start with that larger correctness surface.

2. **A Metal command buffer and encoder per draw.**
   `src/nv2a/nv2a_metal.m:nv2a_metal_draw` creates a command buffer, creates an
   encoder, binds state, ends encoding and commits for each batch. Hundreds
   of draws imply hundreds of submissions per frame. Measure command creation,
   encoding, commit and ring wait separately. Experiment with multiple ordered
   draws per command buffer, flushing at sync/readback and required resource
   transitions. Preserve attachment visibility, guest surface coherence and
   staging-buffer lifetime. Existing slab staging does not remove this cost.

3. **Texture cache hits still read all texture bytes.**
   `texture_buffer` validates hits with full `memcmp`. High hit rate means
   few uploads, not cheap validation. Count bytes compared and elapsed time.
   Avoid caching by pointer alone: guest writes must invalidate cached data.
   A trustworthy dirty-generation scheme is a later optimisation if bandwidth
   measurement warrants it.

## Correctness bugs that can damage pacing

4. **Interrupt scheduling state is shared but no longer protected by default.**
   `bridge_vblank_poll` has shared non-atomic `next_vblank`; the step helper
   has shared `carry_us`. `bridge_device_irq_poll` has shared `next_irq`.
   The global lock only runs in legacy mode; per-vector claims happen later.
   Concurrent waiters can race the deadline check/update even when ISR entry
   itself is exclusive. Protect deadline ownership independently of ISR/DPC
   execution. Test two callers at the same deadline and during a slow ISR.

5. **Deadline drift and coarse retry.**
   Both pumps schedule their next deadline from `now`, incorporating every
   late poll into future timing. Vblank retries also sit behind the next-frame
   deadline. Use a monotonic high-resolution phase and explicitly account for
   missed periods without blindly issuing catch-up interrupts. Test repeated
   late polls and measure wake latency, rather than only average ISR rate.

6. **Auto-reset event consumption is non-atomic and waits poll.**
   `bridge_KeWaitForSingleObject` reads SignalState and then separately writes
   zero. Two waiters can consume one signal; a concurrent set between the read
   and reset can be overwritten. `bridge_KeSetEvent` also uses plain accesses.
   Serialize event state with a host wait mechanism or consistent atomics and
   a wake protocol. Test two waiters/one signal and set-versus-consume races.
   Preserve notification-event semantics and interrupt pumping while blocked.
   Reducing the sleep interval further does not fix these races.

## Suggested order

First establish a valid, scene-confirmed baseline and profile the active render
thread; do not infer CPU idleness from a whole-process tally of blocked workers.
Fix deadline/event races with focused concurrent tests. Then A/B vertex reuse
and command-buffer batching separately. Keep USB/WDH investigation separate
unless time-aligned evidence links it to the frame slowdown. Record frame
latency distributions, sound-server rate, TD retirements and image correctness.

The code and existing logs support substantial CPU-side rendering costs and
fragile scheduling. They do not yet establish the cause of the bimodal split
or quantify the speedup any proposed change will deliver.
