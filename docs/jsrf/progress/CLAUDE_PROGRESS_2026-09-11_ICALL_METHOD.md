# Following the documented method, and what it found in ten minutes

Date: 2026-09-11 (Europe/London), late. After being redirected to the upstream
method rather than continuing to improvise instruments.

## The method, and what we had dropped

`docs/pipeline/06-debugging.md` opens by calling a failed indirect call "the
most common crash type" and prescribes exactly two things for it:

  1. **the caller address**, via `_ReturnAddress()`, mapped back to a function
     through the linker `.map` file;
  2. **the ICALL trace ring**, the last 16 targets, for execution context.

Our `recomp_icall_fail_log` in `jsrf_stock_test/src/recomp_manual.c` had
**neither**. It printed the bad VA and a running total and nothing else -- so
for a whole session it said a garbage pointer was called and never who called
it, which is the one fact that matters. The upstream template
(`templates/new-game/src/recomp_manual.c`) still dumps the ring; the JSRF copy
had been trimmed.

Restored both. The stock tree is not a git repo, so the change is kept here as
`../../diagnostics/jsrf_first_fault/stock_patches/recomp_manual_icall_caller.patch`.
On a mingw build the `.map` step is just
`x86_64-w64-mingw32-addr2line -f -e jsrf_first_fault.exe <caller>`.

## It named the callers on the first run

    [ICALL] Failed to resolve VA 0xFFFFFF00 caller=000000014150BE0B
    [ICALL]   recent targets: FE000190 x7 FE0000B4 0015F9D0 FE0001A8 FFFFFF00 ...

    caller 0x14158FF49  sub_00141E50
    caller 0x14159024F  sub_00141E70
    caller 0x14150BE0B  sub_0013B1C0     <-- the vsync pump
    caller 0x14150C3E9  sub_0013B230

`sub_0013B1C0` is named in `kernel_bridge.c`'s own comment: "its vsync pump
(sub_0013B1C0) is wait for vblank; tick; resume the ADX audio server; repeat".

Ring context, resolved against the XBE thunk table: ordinal 119
(KeInsertQueueDpc) seven times, 145 (KeSetEvent), a guest call, then
**ordinal 3 (AvSetDisplayMode)** -- and immediately after it, the first
0xFFFFFF00.

## The pointer, and why the two hosts differ

`sub_0013B1C0` at 0x0013B1F1:

    eax = MEM32(0x25EFB8);
    if (eax == 0) goto skip;      /* the guest checks! */
    edx = MEM32(0x25EFBC);
    (*eax)(edx);

So guest global **0x0025EFB8** is a function pointer the vsync pump calls only
when non-zero.

    macOS    0x00000000  -> the guest skips the call. Works.
    Windows  0xFFFFFF00  -> non-zero, so it calls, and 0xFFFFFF00 clears
                            RECOMP_ICALL_IS_CODE (>= 0xFE000000) and then fails
                            the kernel lookup.

That also explains the single `skipped not-code target 0x00000000` macOS logs:
a different site, same shape, harmless because the value is zero.

## Where it is NOT coming from

Dumped at load, after the image and the runtime's own writes and before one
guest instruction runs (`RECOMP_DUMP_VA`, added for this):

    macOS    0x0025EFB8 = 0x00000000   0x002648D4 = 0x00000000
    Windows  0x0025EFB8 = 0x00000000   0x002648D4 = 0x00000000

**Identical.** The load is not the difference. So it is written during
execution -- and on both hosts, across full runs:

  - no guest store reaches it (`RECOMP_MEM_WATCH`, which covers the 79,114
    macro stores)
  - no block copy reaches it (`instrument_block_writes.py`, 1571 sites, with a
    positive control proving the instrument fires)
  - no kernel bridge call changes it (`RECOMP_KERNEL_WATCH`)

Three instruments, all silent, on a value that demonstrably changes. **There is
a fourth write path and it is now the whole question.** The candidates are the
ones `recomp_mem_watch.c` excludes in its own header -- runtime and device
writes into guest RAM -- and they run on non-guest threads (IRQ, OHCI, APU,
pusher, NV2A ack) where KWATCH's around-each-bridge-call sampling cannot see
them.

## Next

Extend the watch to the runtime's own writes, which is what its header says it
does not do. The 2,735 uncovered plain guest stores are the other candidate and
are cheaper to enumerate, but they are guest stores on guest threads and would
more likely have shown up in the ordinal/caller trail by now.

Note that 0x0025EFB8 and the XPP list head 0x002648D4 are two different globals
that both end up holding 0xFFFFFF00. One writer that stamps -256 into several
places is a better fit than two coincidences.
