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

## Most of this was already known, and I should have read further first

`STATUS.md` already carried the substance: *"Of the 33 genuine faults on this
host, 20 are the same bug: the DirectSound APU interrupt handler is entered for
an idle-voice trap and dereferences a voice object that is NULL"*, with the
faulting instruction, the register fingerprint, the frame depth, a three-deep
call chain out of the guest's stack, and the handler's registration at
`KeConnectInterrupt(routine=0x001A2681, vector=5)`. That is a better-evidenced
account than this note's, and it predates it.

What is actually new here is narrower, and worth keeping for three reasons:

1. **The exact instruction and operand, at 10 of 10.** `sub_001A2E2E +0x670`
   on guest `0xFFFFFFBE`, identical across A/Bs hours apart on four binaries
   under five switch configurations. A probe can target one instruction.
2. **`ACCURACY_GAPS.md` and `STATUS.md` disagreed**, and the wrong one was the
   one being quoted: "SIGSEGV in the OHCI path" against STATUS's DirectSound.
   Corrected to agree.
3. **It is not a NULL object.** STATUS says the handler "dereferences a voice
   object that is NULL". A NULL object read at `[obj+0xBE]` faults at `0xBE`.
   This faults at `0xFFFFFFBE`, every time — the top of the address space, which
   is what a pointer computed from a **sentinel** looks like. `0xFFFF` is the
   voice list's own terminator. That is a different bug shape from a null
   object, and it points at the link value rather than at the object lookup.

## Which makes ACCURACY_GAPS wrong

`docs/jsrf/ACCURACY_GAPS.md` has carried this as:

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
* **Every fault was in a run that never left the title screen** (`scene=12`),
  and the renderer was therefore barely running in it — which is why none of
  these faults is attributed to today's Metal work.

  **RETRACTED, same day, and it is worth keeping as the error it was.** This
  bullet first said those runs "had already stopped submitting USB transfers",
  read off an `ord175` of 5,600–9,900 against a healthy run's 68,000. That is
  an absolute count compared across runs of different lengths. The crashing
  runs lasted 30–40 seconds because they crashed; the healthy one lasted 270.
  Normalised:

      t2_metalhw1  crashed    40.0 s   117.7 transfers/s
      t1_m5651     crashed    30.0 s   119.4 transfers/s
      t1_metalhw1  healthy   270.0 s   120.5 transfers/s

  **The USB driver was healthy in all three.** The OHCI counters agree — the
  blocked:cleared ratio is 4.05 against 4.01, and `tds_error=0` everywhere. No
  stall, in either direction, and nothing here is evidence about the USB path.

  A real stall looks nothing like this: `pad/gameplay_nobarrage.pad` records 234
  transfers in a 300 s run, which is 0.78/s. Two different failure modes were
  being conflated. `ab_score.py` now reports `usb=N/s` rather than the raw
  count, because the raw count reads as a stall on any run that ended early.
* Two of the ten were in `RECOMP_METAL_HW` arms. Given the other eight predate
  that switch entirely, that is not a signal about it.
