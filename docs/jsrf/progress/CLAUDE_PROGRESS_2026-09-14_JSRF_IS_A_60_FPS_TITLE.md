# JSRF is a 60 fps title, and the "30 fps target" was never measured

2026-09-14. Corrects every handover that says "25 fps against a 30 fps target".

## The measurement

xemu 0.8.136, US ISO, no input, traced with `nv2a_pgraph_flip_stall` — **the
same event our own `[FRAME]` counter is built on**, so the units are identical
and no conversion is involved.

```sh
xemu -trace enable=nv2a_pgraph_flip_stall,file=/tmp/xemu-flip.trace
# count lines over measured wall-clock intervals; the trace has no timestamps
```

| | scene | fps |
|---|---|---|
| **xemu** | boot / logos | 59.1 – 60.1 |
| **xemu** | **in-engine attract demo** (character, HUD, city, dialogue) | **59.2 – 60.1** |
| ours | same attract demo | **11.7 – 14.3** |
| ours | gameplay, Dogenzaka Hill | **31.3** |

11,018 flips traced over ~3 minutes. The dips to 35.3 and 52.8 were logo
transitions; everything else sat on 60.

## What this changes

1. **JSRF targets 60 fps.** The "30 fps target" appears in three handovers and
   nothing measured is behind it. The gap has been understated by 2x throughout.
2. **Our gameplay runs at half speed.** 31.3 fps against a 59.8 Hz vblank, with
   frame time pinned at 31.9–33.5 ms — two vblank periods, the signature of a
   60 Hz title missing its 16.67 ms budget and dropping to every other refresh.
   Titles of this era advance simulation by a fixed step per frame, so half the
   frame rate is half the game speed. That is why it *looks slow* rather than
   merely choppy, and it is what the user reported.
3. **The attract scene is 5x off**, not 2x. 12 fps against 60.

## Where our frame time goes (gameplay, Dogenzaka Hill)

| stage | ms |
|---|---|
| vsh (vertex shader interpreter) | 7.52 |
| submit | 8.41 |
| GPU sync | 0.98 |
| **rest (guest CPU)** | **15.03** |
| total | 31.94 |

The budget for 60 fps is 16.67 ms for the whole frame. Note the guest-CPU half
is now as large as all the graphics work combined, so this is not solely a
renderer problem and should not be attacked as one.

## Method notes

* `nv2a_pgraph_flip_stall` is cheap (~60/s) — unlike `nv2a_pgraph_method` at
  544k/s, it can be traced straight to a file with no FIFO and no gating.
* The trace has **no timestamps**. Count lines over an interval you measured
  yourself.
* **Do not send `quit` to the QEMU monitor** to close a query connection — it
  terminates the VM. It cost one run here.
* Scene-match this like any other comparison: xemu at a logo screen also reads
  60, and proves only that the title *presents* at 60. The number that means
  something is the one taken in a full in-engine scene.
