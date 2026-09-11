# What the macOS build does to run that the Windows build never does

Date: 2026-09-11 (Europe/London), night. Written after the allocators were
exonerated (`CLAUDE_PROGRESS_2026-09-11_ALLOCATORS_EXONERATED.md`) left the
question as "what stops the guest driving D3D at all".

Method: enumerate every platform conditional in `src/` and
`diagnostics/jsrf_first_fault/`, and keep only the ones that change GUEST-
VISIBLE behaviour rather than host output. 19 in `xbox_memory_layout.c`, 16 in
`main.c`, 8 in `nv2a_pb_exec.c`, and a tail of smaller ones.

Most are not it. Every conditional in `nv2a_pb_exec.c` is a Metal hook, the
`__APPLE__` blocks in the memory layout are host mapping mechanics, `apu_core.c`
picks an audio backend, and `main.c`'s are window pumping and crash reporting.
Three things are left, and the first is substantial.

## 1. THE PHYSICAL HEAP ALIAS -- macOS only, and the comment names JSRF

`main.c` calls it under `#if !defined(_WIN32)`, and the function itself refuses
on Windows before doing anything:

    BOOL xbox_EnablePhysicalHeapAlias(void)
    {
    #if defined(_WIN32)
        /* The Windows allocator has a different, high-window backing contract. */
        return FALSE;

The call site says why the title needs it:

    /* JSRF uses low-heap unpinned GPU buffers, then locks them through the
     * CPU physical window. Both views must share bytes. */

On macOS it maps `XBOX_HEAP_BASE..XBOX_HEAP_TOP` into the contiguous window as
a second view of the SAME file mapping, so a write through the low-heap VA and
a read through `0x80000000 + offset` touch one set of bytes.

Measured this evening, same build, same gen, one run each:

    macOS    Physical heap alias: 0x80510000..0x84000000 shares low RAM
             [PUSHER] device fields: +0x24=8056D000 +0x28=805ED000
    Windows  (no alias line at all -- never attempted)
             [PUSHER] device fields: +0x24=80001000 +0x28=80081000

**The macOS push-buffer ring lives inside the aliased range.** 0x8056D000 and
0x805ED000 both sit within 0x80510000..0x84000000. On macOS the guest's command
words, written through the low heap, are visible to the pusher through the
physical window because those are the same bytes.

On Windows there is no alias, so the physical window is storage of its own and
nothing the guest writes through the low heap can ever appear there.

That is the shape of the Windows pusher symptom recorded in
`CLAUDE_PROGRESS_2026-09-11_WINDOWS_BOOTS.md` -- 15,360 dwords consumed, 24
methods recognised, 24 unhandled, one bad header, and a line byte-identical
across 281 samples. A parser reading memory the producer never writes to.

**NOT PROVEN, and the gap matters.** The Windows ring is at 0x80001000, not at
0x8056D000, so its base differs as well as its backing, and this note does not
establish which of those comes first or whether one causes the other. Turning
the alias on and re-measuring is the experiment; predicting the result is not.

**Why it cannot simply be switched on.** Both hosts `VirtualAlloc` the
contiguous window, and macOS then replaces part of that reservation with a
file-backed view -- `MapViewOfFileEx` is `mmap(MAP_FIXED)` on POSIX and will
overwrite pages the process owns. Windows `MapViewOfFileEx` will not map over
an existing reservation; the sub-range has to be released first and then
mapped, which is a real change to the Windows backing contract rather than
deleting an `#if`. That is what the refusal comment is pointing at.

## 2. THE PUSHBUFFER SOFTWARE-METHOD HANDLER -- macOS only

`main.c`, in the pushbuffer ack thread:

    #if !defined(_WIN32) && defined(__aarch64__)
        nv2a_pusher_set_software_method_handler(jsrf_software_method);
    #endif

`nv2a_pusher.c` dispatches method 0x100 only when a handler is installed:

    if (method == 0x100 && param && g_software_method)
        g_software_method(subchannel, param);

So on Windows a software method is silently skipped. On macOS
`jsrf_software_method` raises the PGRAPH interrupt AND blocks the parser until
the guest acknowledges it -- its own comment: "The parser must not execute a
later software method until this one is acknowledged."

Windows therefore never delivers a PGRAPH notify to the guest, and never
applies the ordering the macOS path enforces. If the title waits on a notify,
it waits forever.

Supporting this, the Windows branch of `xbox_memory_layout.c` hardcodes both
ends of that path:

    BOOL xbox_Nv2aRaiseSoftwareMethod(...) { return FALSE; }
    int  xbox_Nv2aSoftwareMethodPending(void) { return 0; }

and `kernel_bridge.c` gates a whole ISR delivery loop on the second one, so
that loop is dead on Windows by construction.

Cheap next measurement, no code change: `RECOMP_PB_NOTIFY_TRACE=1` prints
`[PB-NOP]` for every method 0x100 the parser sees, independently of whether a
handler is installed. If Windows sees software methods, item 2 is live; if it
sees none, item 1 is upstream of it and item 2 cannot be tested until the ring
carries real commands.

## 3. Smaller, recorded for completeness

`xbox_McpxTrapReport` on Windows prints "not this host" rather than counters,
and `g_mcpx_apu_read_trapped` is hardcoded 0 -- consistent with the standing
note that macOS never exercises the APU model's read path.

## A correction to my own .text argument from earlier tonight

`CLAUDE_PROGRESS_2026-09-11_TEXT_QUESTION_SETTLED.md` argued that the ring
cannot reach .text because the 0x80000000 window "is not a view of the RAM
mapping". That is unconditionally true only on **Windows**. On macOS the heap
range *is* a second view of low RAM, via the alias above.

The conclusion is unaffected -- the alias covers 0x80510000 upward and .text is
0x00011000..0x0018CFB0, so .text is aliased on neither host, and the measured
result (zero ring-window pages changed) stands on both. But the reasoning as
written is too broad and would be wrong if reused for an address inside the
heap. Corrected here rather than left to be inherited.

## Where this leaves the plan

The live candidate for "the guest stops driving D3D" is now item 1, with item 2
behind it. Both are *absences* on Windows rather than bugs in shared code,
which is a different shape from the four bugs the oracle found on the way up --
and it is the shape the oracle was built to expose.
