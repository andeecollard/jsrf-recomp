# Windows does allocate. The plan's central fact was a log cap.

Date: 2026-09-11 (Europe/London), night. Step 4 of
`../plans/JSRF_PLAN_2026-09-11_WINDOWS_GRAPHICS.md`, as rewritten earlier
tonight. Measured on both hosts with a new path probe.

## The headline

`JSRF_PLAN_2026-09-11_WINDOWS_GRAPHICS.md` opened with this, and called it "the
one fact that has survived every test":

> `MmAllocateContiguousMemoryEx` (ordinal 166) is called 59 times on macOS and
> zero times on Windows, so D3D never allocates GPU memory and the push buffer
> falls back to a bogus base.

It is not true. Windows calls ordinal 166 and Windows gets memory back.

    site                 macOS   Windows
    E670 entry             221         3
    E670 pre-alloc         221         3
    E670 alloc returned    221         3
    E670 fail return         0         0
    E670 success           221         3
    9760 entry             358         2
    9760 pre-alloc         358         2
    9760 alloc returned    358         2
    9760 fail return         0         0
    9760 success           358         2
    thunk slot 102  0xFE000198  0xFE000198

Not merely non-zero on Windows -- **identical, call for call**. Same heap
descriptors (0x040E47F0, 0x040E52B0, 0x040FFE10, 0x040FFEB0, in that order),
same requested sizes (0x4000, 0x800, 0xC, 0x80000), same thunk. The only
difference is which contiguous address comes back (0x80376000 vs 0x808E6000),
which is allocator state, not behaviour.

Neither function fails once, on either host. **The two allocators are
exonerated completely**, and so is the branch inside them, the heap they take
their descriptor from, and the thunk slot they call through.

## Where "59 and zero" came from

`[HEAP]` lines are **capped**. `heap_trace_limit()` in `xbox_memory_layout.c`
returns `RECOMP_HEAP_TRACE` or **64** by default, and every `[HEAP]` print is
behind it. Counting `ra=0x00199789` and `ra=0x0018E6E9` in a log therefore
counts *log lines inside a 64-line budget*, not allocations. macOS spent part
of its budget on D3D and showed 53 + 6 = 59. Windows dies early, spent the
whole budget on earlier allocations, and showed none.

Zero lines. Not zero calls. The real macOS figure is 579 allocations in 45 s.

This is CLAUDE.md's own rule -- *read a counter's trigger before trusting its
value* -- and it took out the plan's founding measurement, two handovers'
framing, and a statistic of my own from earlier tonight in one go.

## Retracting my own arithmetic from a8afcf5

`CLAUDE_PROGRESS_2026-09-11_TWO_FUNCTIONS_READ.md` said macOS allocates on
"2.6% and 5.4% of entries" and computed P = 0.009 that Windows' zero
allocations were chance. Both rates used the capped line count as a numerator
against a true entry count. The real rate is **100% on both hosts**, so the
probability was never the question.

The conclusion that step 4 as originally written could not work still stands,
and so does the sentence that mattered:

> whatever limits *entry* to these functions is a larger effect than whatever
> happens inside them, and it is upstream of both

That is now the whole story rather than the secondary one. Everything inside
the functions is identical; only the number of times the guest arrives differs,
3 and 2 against 221 and 358.

## What this leaves

The D3D allocation path is not the fault and should not be looked at again. The
question is what stops the guest *calling* these functions after a handful of
resource creations -- i.e. what stops it driving D3D at all -- and that is the
same question as the `0xFFFFFF00` crash, which is still unexplained now that
both the interrupt race and the .text overlap are eliminated.

Three things worth carrying:

  - **Nothing in the ordinal-166 chain diverges.** Do not re-open
    sub_0018E670, sub_00199760, sub_0014A83E, the heap at 0x27DCD4, or thunk
    slot 102. All measured identical.
  - **Re-check every other claim that rests on counting `[HEAP]` lines.** The
    cap is 64 and it is silent. `RECOMP_HEAP_TRACE=<n>` raises it.
  - The push buffer's "bogus base" is still a real symptom, but it can no
    longer be attributed to a failed allocation. Its cause is open.

## The instrument

`diagnostics/jsrf_first_fault/instrument_d3d_alloc.py` installs twelve
observation points into a COPY of a gen tree -- entry, descriptor, pre-alloc,
alloc-returned, fail-return and success for each function -- and
`jsrf_d3d_alloc_probe` / `jsrf_d3d_alloc_report` in `main.c` count them under
`RECOMP_D3D_ALLOC_TRACE=1`. Read-only: every site is a call after an existing
label, taking registers and guest memory as arguments.

The report prints every site **including the zeroes**, deliberately. "The guest
never got here" is the answer this probe exists to give, and a report that
skipped empty rows could not give it.

Entry counters could not have answered this. `sub_0014A83E` and the free are
general-purpose with callers across the title, so their totals say nothing
about these two callers -- which is why this is a path probe and not another
`--va` list for `instrument_func_hit.py`.
