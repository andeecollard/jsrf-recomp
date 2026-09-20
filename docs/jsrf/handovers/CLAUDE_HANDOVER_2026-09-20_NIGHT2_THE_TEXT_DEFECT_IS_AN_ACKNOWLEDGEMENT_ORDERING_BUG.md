# G2 is not a font bug. It is a late vertex read, and it now has a counter

> The title this file was written under named the acknowledgement as the
> cause. The A/B refuted that before the file was finished; section "The
> change" records what happened. The measurement and the mechanism below
> stand -- only the proposed gate died.

20 September 2026, night. One replay, one trace, one three-line change.

## What was measured

`RECOMP_GLYPH_DUMP` already prints, per character quad, the screen rectangle and
the atlas cell it samples. Pointed at the player's own 20 Sep replay on the
player's own switches (`METAL_FF` included), it reconstructs the text the
renderer actually submitted. Decoding is exact: 84 cells per page on a 12x7
grid, codes `0x21..0x75` with `0x60` absent, which is fixed by two facts in one
frame -- `T` (0x54) sits at index 51 and `u` (0x75) at page 0's last cell 83.

At the Garage the reconstruction reads back `This is the GG's Garage.` from the
page-0 pass and `ywzz` from the page-1 pass, at the exact screen positions of
the `y` of "Hey", the `w` of "where's" and the two `z`s of "pizza". The
instrument is sound.

## The defect, in one frame

```
corrupt page-0 draw  q00 x409.2 y60 r0c1      q01 x468.2 y60 r0c3      q02 'm' x325.9
following page-1     q00 x409.2 y60 r0c1='w'  q01 x468.2 y60 r0c3='y'
correct page-0 draw  q00 'G' x302  q01 'u' x314  q02 'm' x325.9
```

The page-0 batch **opens with the quads of the page-1 batch that follows it** --
same position, same cell -- sitting where the `G` and `u` of the `Gum` name
label should be. Those two glyphs therefore sample page 0 and come out as `"`
and `$`, and the label loses its first two characters.

**That is both halves of the reported defect at once.** `$ou` for "you" and
"characters vanish from the start" are the same event seen from two ends, which
the 19 Sep note suspected and could not show.

Over the whole replay, with the instrument counting rather than printing:

| | pre-fix |
|---|---:|
| page-0 text draws | 15,707 |
| draws with a page-1 pass behind them (opportunities) | 2,494 |
| **draws whose head was clobbered** | **149 (5.97%)** |
| glyphs lost | 268 |

It persists for up to ~0.8 s at a time (t=81.9..82.7), which is why it
photographs rather than flickers past.

## Two corrections to the corpus

- **The speech bubble is NOT unaffected.** Every corrupt draw caught here is
  Gum's dialogue box and its name label, on the same two 256x256 Latin pages.
  "The banner font is affected, the bubble font is not" was a sampling artifact
  of a defect that fires on 6% of opportunities.
- **The replay does not reach the spray-can banner.** It produces
  `Grind a rail!`, `Press the A button to jump.` and Gum's dialogue. The two
  named test strings were not the control pair; they were not needed, because a
  corrupt draw beside its correct neighbour is a better control than either.

## The cause

`nv2a_ack_thread` published the acknowledgement BEFORE executing the segment it
was acknowledging (`xbox_memory_layout.c`):

```c
if (!g_nv2a_pusher_owns_dma_get && *get != *put) { *get = *put; }
```

The guest spins on `DMA_GET` catching `DMA_PUT` before it reuses a buffer -- the
wait is quoted in this file at `NV2A_USER_DMA_PUT`. Copying PUT into GET
releases it immediately, so it refills its sprite buffer with the page-1 quads
while the executor has not yet read the page-0 draw's vertices out of that same
buffer. JSRF draws a two-page line as two passes from ONE buffer, refilled from
offset 0, which is why text is where this shows.

The comment above that line already described the failure in the abstract, and
`g_nv2a_pusher_owns_dma_get` exists to prevent it -- **nothing in the tree ever
set it to 1**.

## The change

Move the acknowledgement below the execution, and acknowledge the PUT that was
actually scanned rather than a fresh read of the register. `RECOMP_PB_ACK_AFTER_EXEC=0`
restores the old order for an A/B without a rebuild. Default is on.

## THE FIRST CHANGE WAS DEAD CODE. THE A/B TESTED NOTHING.

Prediction, written before the run: acknowledging only what has been executed
makes the guest wait, so the clobber goes to zero. It scored 149 of 2,494
before and 1,224 of 12,167 after, and that was read as a refutation for about
ten minutes.

It is not a refutation, because **the line that was moved never runs.**
`nv2a_ack_thread`'s `GET := PUT` is guarded by `!g_nv2a_pusher_owns_dma_get`,
and `main.c:1348` sets that flag to 1 at the top of `jsrf_pushbuffer_ack`,
which is live in both runs -- 169 and 171 `[PB-ACK]` reports in their logs. The
two numbers are two different scenes with identical behaviour.

Check the flag is reachable before spending a run on the branch it guards.
`RECOMP_PB_ACK_AFTER_EXEC` is kept, defaulted **off**, and is untested.

## THE FENCE THAT IS ACTUALLY READ, AND THE ONE THAT IS ACTUALLY LIED TO

`jsrf_pushbuffer_ack` runs a hot loop -- 95,000 iterations a second, measured --
and publishes progress to two places:

| fence | published as | honest? |
|---|---|---|
| NV2A register GET (`0xFD800044`) | the parser cursor, once per step, inside `jsrf_pb_poll` | **yes** |
| the guest's D3D fence at `[dev+0x34]`, which its pushbuffer reserve spins on | the whole of `submitted`, as soon as the poll returns | **no** |

`jsrf_pb_poll` already argues the case for the register: "A poll can cover most
of the ring -- 26,920 dwords in the run that caught this -- and for all of that
time the producer sees a GET that has not moved, so it is free to fill the ring
and write over the very bytes being parsed." The guest's own fence never got
that treatment.

A poll exits with work outstanding for ordinary reasons: the 16-segment cap, a
PUT outside the ring bounds, a step boundary at `JSRF_PB_STEP`. Each one hands
the guest a fence past the parser.

## What is left, and it is narrower than before

The measurement stands: the page-0 draw is rasterised from a buffer the guest
has already refilled. Nothing makes the guest wait, so the fix cannot be a
handshake -- it has to make our execution PROMPT. Real hardware's pusher is
microseconds behind the producer. Ours is one `nv2a_ack_thread` iteration
behind, and that iteration also does the register acks, `xbox_McpxHoldRegisters`,
`ohci_periodic_tick`, `fence_mirrors_tick`, `frame_counters_tick` and
`framebuffer_probe_tick` before it ever looks at PUT.

The next measurement, already in the tree and awaiting a build:

1. **`[PB-FENCE]`**, a counter in that loop, reports acknowledgements published
   past the parser's cursor. If it reads zero the theory is dead without
   anybody building a fix.
2. **`RECOMP_PB_HONEST_FENCE=1`** publishes the cursor instead, carrying
   `submitted`'s high bits so the value stays in the guest's address space, and
   leaving the subroutine case alone because a cursor inside a called buffer is
   not a ring address. Off by default.

Note the MMIO write hook is NOT an option on this host: `nv2a_mmio_hook.c` is
`#if defined(_WIN32)`, a VEH instruction decoder, and the Mac has no equivalent
in this tree.

The counter is the thing to keep either way: `clobber.py` scores any run's
`runtime.log` with no picture and no player, and a clean run is exactly 0.

## Why this is worth more than a text fix

Nothing in it is about fonts. Any guest buffer reused inside one frame is read
late the same way, and this is the first defect that pins that to a measurement
instead of a suspicion.
