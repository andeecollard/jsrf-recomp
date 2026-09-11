# Reading the two D3D allocators, and what the counts actually say

Date: 2026-09-11 (Europe/London), late. Follows
`../plans/JSRF_PLAN_2026-09-11_WINDOWS_GRAPHICS.md`, whose step 3 this
executes and whose step 4 it retires.

No runs were taken. Everything below is a static read of the gen tree, a
read of the XBE import table, and re-analysis of logs that already existed in
the previous session's scratchpad.

## What the two functions are

`sub_0018E670` (0xBB bytes) and `sub_00199760` (0x56 bytes) have the same
shape, and it is simpler than the plan assumed. Neither contains a capability
check, a mode field, or any test that could plausibly differ between hosts.
Each does exactly this:

    descriptor = sub_0014A83E(0x40, tag)      // tag 0x14 / 0xC
    if (!descriptor) return 0x8007000E;       // E_OUTOFMEMORY, no allocation
    mem = (*(void**)0x001C40F8)(size, 0, 0x07FFFFFF, 0x80, 0x404);
    if (!mem) { free(descriptor); return 0x8007000E; }
    descriptor[1] = mem & 0x03FFFFFF;         // physical address
    descriptor[0] = 0x01040001 / 0x01000001;  // type tag

So there is **one** branch before the allocation, and it is the same call in
both: `sub_0014A83E`, a two-line wrapper over
`sub_001497DC(heap = MEM32(0x27DCD4), flags, size)`.

`0x001C40F8` is **kernel thunk slot 102**. Read straight out of the XBE at raw
offset 0x1B4198 it holds `0x800000A6` — **ordinal 166**,
`MmAllocateContiguousMemoryEx`. That is now confirmed from the image rather
than inferred from the slot-93 arithmetic in the oracle handover. Both call
sites reach ordinal 166 through this one slot and through no other path, and
the argument order matches the kernel prototype exactly.

## The icall is not what is failing

`RECOMP_ICALL_SAFE` sets `eax = 0` on every failure — unresolvable target, or a
target outside the code range. Both functions then read `eax`, see zero, and
return `E_OUTOFMEMORY`. A failing icall is therefore *indistinguishable to the
guest* from a genuine out-of-memory, which makes it a good suspect.

It is not happening. Both failure paths log unconditionally on first
occurrence (`recomp_icall_fail_log`, `recomp_icall_not_code_log` in
`jsrf_stock_test/src/recomp_manual.c`), so the existing logs already answer it:

    Windows   [ICALL] skipped not-code target 0x00000000 (1 times)
    macOS     [ICALL] skipped not-code target 0x00000000 (1 times)

Exactly one on each host — it is not a Windows-only event, and it is not a
discriminator. In `win-id.log` it also arrives *after* `ISR 0xFFFFFF00 not in
dispatch`, i.e. after that run had already died in the `KeConnectInterrupt`
race, so it is a consequence of the race and not of the D3D path.

Retraction, recorded because it nearly became the finding: the null icall
looked like the answer for about ten minutes, on the strength of a Windows log
containing one and the first macOS log I grepped containing different targets.
Counting it on both hosts is what killed it. The absence-measurement rule in
CLAUDE.md applies in the other direction too — a presence needs a control.

## The counts do not support step 4

The plan's step 4 was to instrument the arguments of the two functions and diff
them across hosts. The sample size does not permit it.

From one run per host, taking the last contiguous `[FUNC-HIT]` table on each
(caution (c) in the oracle handover), and with the `ordinal 166` allocations
counted in the *same* logs:

    sub_00199760   mac 53 allocations / 979 entries = 5.41%   win 0 / 77
    sub_0018E670   mac  6 allocations / 235 entries = 2.55%   win 0 / 15

If the branch inside the functions behaved identically, Windows' 92 entries
would be expected to produce **4.6** allocations. Observing zero has
probability 0.0093 — about 1 in 107. That is suggestive, but it is one run, and
the whole of the `sub_0018E670` half is worthless on its own (P = 0.68).

Instrumenting arguments would sample 92 Windows calls against a 5% event rate.
It cannot settle anything. Step 4 is retired.

## The scale control, which inverts the framing

The plan and both handovers describe Windows as walking in "a handful of
times". Normalising against the 547 sites armed on both hosts says the
opposite about the machine as a whole:

    001C288F   mac 64,734     win 350,722     win runs 5.4x MORE
    001A71B3   mac 64,734     win 350,718     win runs 5.4x MORE
    0018CE50   mac 38,373     win 246,062     win runs 6.4x MORE

Windows is not idle, short, or starved. It spins in the interrupt and poll path
at five to six times the macOS rate. Against that baseline the two allocators'
raw 13-16x deficit is nearer 70-90x normalised, and the draw path is gone
outright: 187 sites armed on both hosts are present on macOS and have literally
zero entries on Windows, headed by `001BAB20` at 17,126,676 macOS calls.

**Do not read that list as a lead.** macOS reached gameplay and Windows is
stuck in D3D init, so everything downstream of a working renderer is absent by
construction. It is a ceiling on where the fault is, not a location — the same
caution the plan closes with. Its only real use is the direction of the scale
correction above.

## The one link never measured

`sub_0014A83E` is armed in no run, on either host — it does not appear in any
`[FUNC-HIT]` table in the corpus. It is the only untested step in the whole
chain, it is shared by both functions, and it discriminates the two surviving
explanations with a single site:

  - it returns non-NULL on Windows → the guest reaches the ordinal-166 icall
    and that call returns zero, which given the section above would have to
    come from inside `bridge_MmAllocateContiguousMemoryEx` or
    `xbox_ContiguousAlloc`;
  - it returns NULL on Windows → the guest never reaches the icall at all, and
    the fault is the title's own heap (`MEM32(0x27DCD4)`) on this host, which
    is upstream of D3D entirely and would explain the entry deficit too.

Arm `0x0014A83E`, `0x001497DC` and log `MEM32(0x27DCD4)` once at startup. Three
sites, one run per host.

That said, 15 and 77 entries against 235 and 979 means whatever limits *entry*
to these functions is a larger effect than whatever happens inside them, and it
is upstream of both. The heap branch above is the only one of the two
candidates that would account for both observations at once, which is why it is
worth the run before anything wider.

## Still true, still first

Nothing here displaces steps 1 and 2 of the plan. The `.text` checksum settles
whether any of these Windows measurements mean anything, and the
`KeConnectInterrupt` race demonstrably killed `win-id.log` before it could
report. The corpus re-analysis above was free; those two still want the build.
