# The crash is one instruction, and it is in the voice list

15 Sep 2026, evening. Found while watching the renderer A/Bs for regressions,
which is not what it is about. No code changed for this note.

## The finding

Every guest fault recorded across today's scripted runs is the same one:

    10 x  HOST PC: sub_001A2E2E +0x670
    10 x  HOST FAULT ADDRESS: 0x00000070FFFFFFBE   (guest 0xFFFFFFBE)

Not the same function. **The same instruction, faulting on the same address,
every time.** Ten faults across A/Bs taken hours apart, on four different
binaries, under five different switch configurations.

`0x1A2E2E` is inside the **DSOUND** section — `VA=0x0019E340 vsize=116060`,
so `0x19E340`–`0x1BA89C`. That is Microsoft's DirectSound, recompiled as guest
code with the rest of the title.

## Which makes STATUS.md wrong

`docs/jsrf/STATUS.md` has carried this as:

> ~13% of runs SIGSEGV in the OHCI path

It is not the OHCI path. It is one instruction in recompiled DirectSound. The
OHCI attribution appears to have come from runs whose *other* symptom was a
stalled USB driver — which is real and is discussed below — rather than from a
fault address.

## And a prediction from this morning is now confirmed

`docs/jsrf/progress/2026-09-15-audio-lifecycle.md:38-41`, written from static
reading of the generated C and explicitly flagged as not yet observed:

> `sub_001A2E2E` reads software previous/next links and updates the hardware
> list. A NULL object or bad software link can lead to the observed low-address
> dereference. Stack membership and last trap handle alone do not establish
> that the object was freed.

That is the function. It is now observed ten times at one instruction, so the
"bad software link" half of that prediction has evidence behind it and the
freed-object framing — listed in the 15 Sep handover's retractions as *"Still
unobserved"* — can stop being a hypothesis about stack membership and become a
question about one dereference.

One correction to the prediction while confirming it: the address is **not
low**. `0xFFFFFFBE` is the top of the guest address space, which is what a
computation from a sentinel looks like rather than what a NULL object looks
like. `0xFFFF` is the hardware voice list's own terminator.

## Why this matters beyond stability

`sub_001A2E2E`'s job — reading the guest's software previous/next links and
updating the hardware list — is the exact correspondence this whole day's audio
work is about. Today established that our `regs[top]` and the guest's own
software list **disagree about the head**: the guest re-ONs a voice that is
already the head, our insert overwrites that voice's live successor with its own
handle, and the walk pins on a one-entry cycle.

So the two open defects are plausibly one defect:

* the **trap storm** is what the divergence does when the guest survives it;
* this **fault** is what it does when the guest computes a node pointer from a
  link it did not expect.

Not proven to be one cause. But they are the same function's data, and a
sentinel-shaped faulting address is the shape that divergence would produce.

## What would settle it

The probe that note already specified, and which is still the right one:
capture the lookup pointer and the early-return conditions in `sub_001A241F` /
`sub_001A200D` / `sub_001A2E2E`, then follow the writer that changes that
pointer. What is different now is that the target is one instruction rather
than a function family, so the probe can be narrow: dump the software prev/next
link values on entry to `sub_001A2E2E`, and the fault becomes reproducible
evidence rather than a rare event.

Do NOT act on this by suppressing a trap or forcibly unlinking a voice. That
was the standing instruction in the audio-lifecycle note and it is still right:
patching around the symptom hides the writer.

## Scope and caveats

* Ten faults is ten, not a rate. Today's per-A/B counts were 3/4, 2/6, 1/6,
  1/4, 1/2 and 0 in four other A/Bs; that is consistent with the ~12% the
  project already records, and nothing here re-measures it.
* **Every fault was in a run that never left the title screen** — `scene=12`
  with `ord175` in the 5,600–9,900 range, i.e. the USB driver had already
  stopped submitting transfers. Whether the stall causes the fault, the fault
  causes the stall, or both follow from something earlier is not established
  here. It does mean the renderer was barely running in every crashing run,
  which is the reason these faults are not attributed to today's Metal work.
* Two of the ten were in `RECOMP_METAL_HW` arms. Given the other eight predate
  that switch entirely, that is not a signal about it.
