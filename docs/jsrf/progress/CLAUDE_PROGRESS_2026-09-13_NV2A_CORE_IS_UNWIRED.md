# nv2a_core.c is unwired scaffolding, and the case for wiring it is not yet made

13 Sep 2026. Audit prompted by a good question: are we reimplementing things the
xboxrecomp libraries already provide?

## What is linked, and what is not

Measured at the linker, not by grep -- which archive members the linker actually
pulled into `jsrf_first_fault`:

    xbox_kernel    23/25 objects      xbox_nv2a     2/6   <-- mostly unused
    xbox_apu        6/6               xbox_video    2/3
    xbox_vsh        4/5               xbox_dsound   0/1   <-- entirely dead

`nv2a_core.c` (798 lines) is NOT linked. Neither is `nv2a_mmio_hook.c`.
`nv2a_init_standalone` has never been called by anything: `git log -S` finds it
only in the original import (7a71ed2), and the only references in the tree are
two READMEs using it as example code. It was never wired up -- not tried and
abandoned.

## What it would give us, and what it would not

Mostly COMPLEMENTARY to what we have, not duplicated:

  * nv2a_core is the MMIO REGISTER-BLOCK model: PMC, PBUS, PTIMER, PFB, PCRTC,
    PRAMDAC, PVIDEO, PFIFO read/write handlers, `nv2a_update_irq`, DMA object
    load/map. We have none of these as such.
  * nv2a_pb_exec.c (3354 lines) is the PUSHBUFFER METHOD interpreter. nv2a_core's
    `pgraph_method` is a stub dispatcher that renders nothing. So the executor
    was NOT redundant work.

The one real overlap is the interrupt path, and nv2a_core's version is better by
design. It COMPUTES the PMC summary from per-engine state:

    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;

We instead STORE interrupt bits in guest RAM and run a background thread masking
them against a carve-out table (xbox_memory_layout.c:240-245). The comment above
that table records what that costs: the ack thread raced the vblank raise, PMC
bit 24 read back zero before D3D's DPC sampled it, and the run ended with
"exactly one ISR and one DPC per run, then every D3D thread blocked forever".
That race is structurally impossible in a computed model.

## Why it is not being wired up today

**The failure it would prevent is not happening.** `[VBLANK]` already reports the
handshake health, and across six runs today:

    unacked_skips=0  not_ready=0     (VBL, SIL, FIXED, PEEK, SCHED, RANGE2)

Both counters are the direct instrument for that race, and both are clean.

And the change is not small. Our NV2A registers live in guest RAM and reads are
ORDINARY LOADS -- only writes trap. Making the model authoritative means
trapping NV2A register READS the way the APU aperture does (PAGE_NOACCESS over
MCPX_APU_MODEL_SIZE). The APU read trap alone costs 164,744 faults in a 154 s
run; NV2A registers are read far harder than that, by the pusher and by every
DPC. So this is a structural change to a load-bearing, currently-working
subsystem, with a real and unmeasured performance cost, aimed at a bug the
instruments say is not occurring.

That is the definition of speculative churn, and this project has a rule against
it: do not patch around a gate until a run proves which value is wrong.

## When to revisit

Wire it up if any of these appear:
  * `unacked_skips` or `not_ready` stops reading zero
  * a title needs PTIMER, PVIDEO or PRAMDAC, none of which we model at all
  * the derived-summary race reappears under a different scene or host

The contained first step, if that day comes, is `nv2a_update_irq`'s LOGIC only
-- computing the summary at raise and ack time against the RAM-backed registers
-- keeping nv2a_pb_exec.c as the executor and not introducing read traps. That
gets the correctness property without the page-fault cost.
