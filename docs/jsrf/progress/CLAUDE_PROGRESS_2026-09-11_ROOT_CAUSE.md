# 0xFFFFFF00 is a depth-clear value, and we were writing it ourselves

Date: 2026-09-11 (Europe/London), late. Root cause of the crash that ended
every Windows oracle run.

## What it was

`clear_surface` in `src/kernel/nv2a_pb_exec.c` -- **our own NV2A push-buffer
executor** -- clears a Z24S8 depth surface byte by byte:

    if (param&2) p[0]=(uint8_t)value;                             /* stencil */
    if (param&1) for (k=1;k<4;++k) p[k]=(uint8_t)(value>>(8*k));  /* depth   */

Stencil 0x00 followed by depth 0xFF 0xFF 0xFF is, little-endian,
**0xFFFFFF00**. The value chased all session was never a guest computation, a
stack leak, an unbridged ordinal or a bad pointer. It is a cleared depth pixel.

On Windows the depth surface resolves to **guest 0x00248000**, 0x12C000 bytes
(640x480x4), which lies inside the loaded image (0x00011000..0x00288620). So
every depth clear painted 1.2 MB of 0xFFFFFF00 across the title's own .data,
including both globals this investigation had been chasing:

    0x0025EFB8  a function pointer the vsync pump (sub_0013B1C0) calls if non-zero
    0x002648D4  the XPP list head that sub_001BFA3A walks

macOS places the same surfaces above the image and is untouched.

## How it was found, and why nothing else could have

Three instruments were silent on both globals across full runs: the guest-store
watch (79,114 macro stores), the block-write probe (1571 lifted string ops, with
a positive control), and the kernel-call watch. Correctly so -- the writer was
none of those. `recomp_mem_watch.c` says in its own header that it does not
intercept kernel, HLE or device writes, and this was a device model, on the
pusher's thread.

What found it was `RECOMP_STORE_WATCH`: a host page guard over ordinary guest
RAM, reusing the VEH store decoder that already serves the device pages, so the
faulting store is reported WITH ITS HOST PC and then completed. Four consecutive
PCs, one addr2line, and the writer had a name and a line number.

Two things about that instrument are worth keeping:

  - **It must not share the device lock.** The first version took
    `g_nv2a_pcrtc_lock`, which the pusher takes to raise a PGRAPH notify. Two
    runs then stalled before AvSetDisplayMode, which reads as "the guard
    prevents the corruption" and actually means "the guard wedged the title".
    It has its own lock now.
  - **Check the run got far enough before believing a zero.** Those same two
    runs reported no writes to the watched range, and the reason was that they
    never reached the code that writes it.

## The guard, and what it is not

`nv2a_range_hits_image` refuses any surface write that overlaps the loaded
image, and says so once per distinct range:

    [NV2A] REFUSED a surface write over the loaded image:
    guest 0x00248000..0x00374000 overlaps image 0x00011000..0x00288620

This is a missing invariant, not a workaround: the GPU has no business writing
the title's code or data on any host, and the DMA checks above it validate the
offset against the DMA object's own limit, which says nothing about where that
object was set up. But **it is not the root fix**. The root bug is that the
depth surface address is 0x00248000 on Windows and correct on macOS -- the same
family as the framebuffer landing at 0x001B2000 and the push-buffer ring at
physical 0x1000. Something in this host's allocation path hands the title
addresses that collide with its own image. The log line says so on purpose.

## Result

    macOS    zero refusals. 2,729,533 methods, 7515 clears, 1882 flips,
             ctest 21/27 -- unchanged.
    Windows  crash GONE. No 0xFFFFFF00 anywhere, no guest fault, still running
             at 170s where every previous run died inside 60.

The next boundary is already visible and is a better one: a PGRAPH software
method with parameter=2 is raised and never acknowledged --
`pmc=00001000 intr=00100000 fifo=00000000`, so the error bit is set and the
guest has not restored FIFO access. parameter=9 notifies complete normally.

## Hypotheses this closes

Every one of these was tested and eliminated before the answer turned up, and
none of them was it: .text corruption by the ring, the KeConnectInterrupt race,
concurrent guest DPCs, the OHCI 0xFF memset, unbridged ordinals leaking stack,
guest block copies, and kernel bridge calls.

---

# Addendum: the real fix, one field wide

The refusal guard made the corruption visible and non-fatal. This is why it
happened, found by printing the surface address in its two parts.

    macOS    [SURFACE] zeta dma_handle=0x0A base=0x00000000 limit=0x07FFAFFF
                       pitch=2560 offset=0x007B4000 -> 0x007B4000..0x008E0000
    Windows  [SURFACE] zeta dma_handle=0x0A base=0x00000000 limit=0x07FFAFFF
                       pitch=2560 offset=0x00248000 -> 0x00248000..0x00374000

Same DMA object, same base, same limit, same pitch. **Only the offset differs**,
and the offset is the guest's own SET_SURFACE_ZETA_OFFSET -- so the guest had
allocated its depth buffer somewhere different, and the GPU model was faithfully
clearing where it was told.

`xbox_ContiguousAlloc` is two different allocators:

    POSIX     xbox_HeapAlloc(size, alignment)      -- the guest heap, which
                                                      already sits above the image
    Windows   g_contig_next, starting at XBOX_CONTIG_BASE

so on Windows the first contiguous allocation has physical offset 0 and they
climb from there, straight through the title's own code and data. The GPU
addresses this window BY PHYSICAL OFFSET, so anything below the image end is
aliased onto the image.

The POSIX branch's own comment records this exact bug being fixed there, for
this same title:

    Returning 0x80084000 here made JSRF's raster clear write to 0x00084000
    (live guest code) instead of its framebuffer.

It was fixed on one host and left standing on the other -- the same shape as
every other find of this session.

## The fix and what it moved

The Windows arena now starts above the loaded image, read lazily because the
image bounds are only known after the sections load:

    [CONTIG] arena starts above the image: 0x80290000 (image ends 0x00288620)

    depth surface   offset 0x00248000 -> 0x004D8000
    framebuffer     fb=0x001B2000     -> fb=0x00442000

Both now clear the image. Zero refusals, zero 0xFFFFFF00, no guest fault, and
the title gets far enough to load its media -- the run ends in
`Media\People\People01.dat` (1,157,120 bytes read) and then an assertion in our
own APU voice processor, `v < MCPX_HW_MAX_VOICES` at apu_vp.c:1055. That is a
different and much later failure, and it is the next thing to look at.

macOS unchanged and untouched by this (its branch is the POSIX one): 2,733,864
methods, 7528 clears, 1885 flips, 61.8 Hz, zero CONTIG lines, ctest 21/27.

## Standing conclusion

Three symptoms had one cause. The depth surface inside .data, the framebuffer at
0x001B2000, and the push-buffer ring at physical 0x1000 were all the Windows
contiguous arena starting at offset 0. The ring is worth re-measuring now.
