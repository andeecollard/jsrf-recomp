# The USB stall: a guest store that never faulted

14 Sep 2026. Binary `-O2`, gen `f0d5dca6a9a4a3b9` (current), `RECOMP_METAL_BATCH=0`.
Fix in `6aaf09f`.

## Verified

**The controller stopped because the guest stopped being told to look.**

`HcInterruptEnable` is write-1-to-set and the MCPX trap models that. But to
perform the store, the handler made the guest's own page writable across two
`VirtualProtect` calls, and so did every other register writer. A guest store
landing inside that window completed as a plain store with no semantics. The
guest re-arms the master enable constantly — thousands of times a run — and
that write, landing untrapped, replaced the whole register with `0x80000000`,
taking `WritebackDoneHead` with it.

Caught in the act, with the trap's own expectation as the oracle:

    trap left 00000073, register reads 80000000
      -- a guest store reached the page without faulting

`0x80000000` is a value healthy runs never produce: 356 mask changes in one
run, over exactly four values (`80000073`, `00000073`, `80000077`, initial 0).

The consequence is visible at the stall. The snapshot shows the controller
`UsbOperational` with every list enabled, no doorbell pending, the pad's
interrupt ED correctly scheduled in four of the thirty-two HCCA slots — and
`HeadP == TailP`, no TD queued. Upstream of that, `HccaDoneHead` is non-zero
and unclaimed with WDH set and unacknowledged. Not "queued work unserviced";
**completed work unreclaimed**.

And the guest-side counter says so directly. `wdh_cleared` counts the driver
acknowledging the done queue — guest work, nothing our injector can fake:

| run | guest acknowledgements |
|---|---|
| `ab-base-1` (pre-fix) | 1032, 2117, 2764, then **2764 for 26 reports** |
| `ab-base-4` (pre-fix) | to 8068 by report 8, then **frozen for 21** |
| `ab-batch-4` (pre-fix) | to 17236 by report 17, then **frozen for 12** |
| `fixval-1` (post-fix) | 1062 → **31381**, monotonic, 29 reports |
| `fixval-5` (post-fix) | → **31950**, monotonic, 29 reports |

The freeze is permanent once it starts, which is what a masked-off interrupt
predicts: nothing ever wakes the driver again.

## The fix

The aperture is backed by a file mapping and mapped **twice** — the guest's
view at the native address, guarded and never unprotected, and a private alias
that is always writable. The runtime writes through the alias. Protections are
per-mapping and the pages are the same pages, so there is no window and nothing
to race.

Register semantics are untouched: what changed is where the computed value is
stored, not how it is computed. No periodic mask restoration, no forced
interrupts, nothing that would hide the symptom. The machinery was already
here — guest RAM is aliased this way for the mirrors. Without a successful
double mapping it falls back to the old path and says so in the log.

`mcpx_hw_store_n`'s comment said windows "cannot be eliminated while the
mechanism is mprotect, so the rule is to open as few as possible". They can be;
this removes the constraint rather than living inside it.

## Validation

Nine runs, 300 s each, `RECOMP_METAL_BATCH=0`, stall snapshot and bypass
detector armed, scored on guest-side counters.

| | pre-fix | post-fix |
|---|---|---|
| full runs that froze | **10 of 12** | **0 of 7** |
| bypasses | 1 caught | 0 |
| mask ever `80000000` | at every stall | 0 |
| WDH-enable losses | yes | 0 |

Seven full runs reached gameplay (scene 70–76) and ran to completion, TDs
34,195–35,198 and acknowledgements 30,654–31,950, climbing through all 29
reports. Two ended early on the pre-existing intermittent guest crash, healthy
to that point. At the old freeze rate, seven clean runs in a row is about
4 in a million.

Tests: 28/30 (the two failures are the deliberately-red lifter gates, see
`CMakeLists.txt`). Focused set — `jsrf_ohci_register`, `jsrf_ohci_transfer`,
`jsrf_kernel_protect`, `jsrf_pgraph_notify`, `jsrf_apu_register` — 5/5.

## Hypothesis, not verified

**That the bypass caused the original stall is not proven.** The one capture of
it landed in the last fraction of a second of a run that never stalled. What
exists is: the mechanism caught in the act, a register state at the stall that
healthy runs never produce, a freeze signature that the mechanism predicts, and
a before/after contrast across nineteen runs. Not a single run in which the
bypass is observed and the freeze follows. That run is what would close it.

## Two bugs introduced here, both caught by tests rather than by reading

`mcpx_w` translated NV2A faults as well — the same handler serves them and
their addresses are nowhere near this mapping — so those stores went to a wild
offset; `jsrf_pgraph_notify` failed at once. And the handler skipped the
unprotect whenever an alias existed, including for NV2A pages, which would have
left them faulting for ever.

The bypass detector also had to move onto the faulting thread and inside the
lock. On the frame tick it sampled the register and the expected value from
another thread, so a tick landing between the guest's paired
`HcInterruptDisable(80000000)` and `HcInterruptEnable(80000000)` saw a
half-applied toggle and reported a bypass that had not happened.

## Still open

- The intermittent guest crash, ~2 runs in 9 here, pre-existing.
- `wdh_cleared` proves the guest consumes completions. It does not prove the
  game acts on the input in them; that needs a guest-visible response.
- `[PAD-TRACE]` remains our own injector and is not evidence either way.
