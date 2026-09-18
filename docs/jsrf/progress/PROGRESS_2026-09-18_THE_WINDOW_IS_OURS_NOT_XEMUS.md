# The raise/dispatch window is ours, not xemu's — 18 September 2026

Read xemu's MCPX APU against ours, because our APU *is* xemu's
(`src/apu/README.md`: "extracted from xemu") and the DSOUND crash is a
time-of-check/time-of-use between our raise and the guest's ISR. The question
was whether we inherited the window or built it.

We built it.

## What is inherited, and is therefore not the bug

The idle-voice trap is xemu's, line for line:

- `vp.c:1835` raises `fe_method(d, SE2FE_IDLE_VOICE, v)` from the same
  voice-list walk, for the same reason — the voice is linked and
  `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` is clear.
- `vp.c:559-570` handles it by setting `FECTL` to `TRAPPED` with
  `FETRAPREASON_REQUESTED` and `d->set_irq = true`. Ours
  (`src/apu/apu_vp.c:2677`) is the same statements plus a counter.

So "we invent interrupts xemu does not" is dead. Do not re-derive it.

## What diverges — the whole of it

| | xemu | us |
|---|---|---|
| `pci_irq_assert` | real QEMU PCI IRQ | **`{ (void)d; }`** (`src/nv2a/qemu_shim.h:195`) |
| serialisation | `bql_lock()` around `update_irq` (`apu.c:279-285`) | none; no guest-execution lock exists |
| delivery | guest takes it at the next instruction boundary | polled by `bridge_device_irq_poll` |
| trigger | the device decides | a guest thread calls `KeWaitForSingleObject` |
| floor | microseconds | `BRIDGE_DEVICE_IRQ_PERIOD_MS = 8` (`kernel_bridge.c:3924`) |

xemu drops the device mutex, takes the Big QEMU Lock, asserts a real IRQ, and
the vCPU takes it almost at once. We assert nothing — the stub discards its
argument — and delivery waits until some guest thread happens to block, then no
more often than every 8 ms.

**That is the window.** Not a subtle fault in the voice processor we inherited:
a delivery mechanism orders of magnitude slower than the hardware DirectSound
was written against. The ISR at `001A200D` reads `this->owner[h]` with no NULL
check and is entitled to — on hardware the trap is serviced before the teardown
that clears it can run. On our timing it may not be serviced for milliseconds,
and `owner[0]` goes NULL inside the gap.

This is consistent with every measurement already taken, and explains the one
that looked strangest: `owner[h]` at the raise reads 27,937 non-NULL to 9 NULL
and **0 of those NULLs were h==0**, yet all 26 crash dumps name voice 0. The
value is right when we look and wrong when the guest looks, because of how long
"when the guest looks" takes to arrive.

## What this does NOT claim

The crash rate has not been measured against a shorter window. The mechanism is
argued from source on both sides and fits the existing counters; it is not
demonstrated. The measurement is the experiment below, and until it runs this is
a hypothesis with a good pedigree and nothing more.

## The experiment, which is a config change and not a patch

`RECOMP_IRQ_THREAD` (`kernel_bridge.c:3990`) already exists: a dedicated host
thread running `Sleep(1)` then the timer and device pumps. It is compiled into
the macOS binary — verified, five `[IRQ-THREAD]` strings, no `_WIN32` guard —
started from `kernel_bridge.c:7856`, and it has never been switched on here.

Arming it moves delivery from "whenever a thread blocks, at most every 8 ms" to
"every 1 ms". If the window is the mechanism, the crash rate falls. If it does
not move, the window is not the mechanism and the dispatch-time check earns its
turn.

Its positive control is the `[IRQ-THREAD]` banner it prints at start: absent
means the thread never ran, and a crash count from such a run measures nothing.

**Read the grammar before setting it.** The gate is
`if (started || !getenv("RECOMP_IRQ_THREAD")) return;` — a bare `getenv`, so
**`RECOMP_IRQ_THREAD=0` turns it ON**. Only removing the line turns it off.
This is exactly the hand-rolled-switch hazard `switch_audit.py` ratchets
against, and it is live in the one switch we now want to A/B.

Watch `[APU-IDLE-EDGE]` alongside. Delivering four to eight times more often
could feed the idle-trap storm, and the re-raise counters are where that shows.

## A note on FEDEC_HOLD, which is ahead of upstream

xemu writes the decode pair unconditionally at the top of `fe_method`
(`vp.c:172-173`), directly beneath this:

```c
//assert((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == 0);
```

Its authors met "a method arrived while the front end was trapped" and
commented the assertion out rather than handling it. That is precisely what
`RECOMP_APU_FEDEC_HOLD` was built for, so the decode-pair race is real,
acknowledged upstream, and unhandled there. Our hold is not a workaround for
something we broke.
