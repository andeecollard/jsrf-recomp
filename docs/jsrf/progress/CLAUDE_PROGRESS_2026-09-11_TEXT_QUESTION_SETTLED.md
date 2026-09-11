# The .text question, settled three ways, and the race that was not the crash

Date: 2026-09-11 (Europe/London), night. Executes steps 1 and 2 of
`../plans/JSRF_PLAN_2026-09-11_WINDOWS_GRAPHICS.md`. Instruments in `db7d9e4`,
Windows runs taken here.

## THE GUEST IS NOT OVERWRITING ITS OWN CODE. Step 1 is closed.

The plan opened with this and made everything else conditional on it:

> The Windows ring bounds (0x1000-0x81000) overlap the title's .text
> (0x11000-0x18CB30) by 448 KB. If the guest is writing command words over its
> own code, every later measurement is untrustworthy and that is the whole
> story.

It is not the story. Three independent lines, and they agree.

**1. Measured.** `RECOMP_TEXT_CHECKSUM=1` on Windows, 8 pages inside the ring
window and 8 outside as a control:

    [TEXT-CK] page 13 VA 0x001C3000 (control) CHANGED ...: 1 dwords,
              VA 0x001C3F20..0x001C3F20, first 0x305F554D -> 0x375F554D
    [TEXT-CK] 1 page(s) changed, all OUTSIDE the ring window

Zero of the eight pages inside the window move. The one page that does is
`0x001C3F20`, little-endian ASCII `"MU_0"` -> `"MU_7"` -- a memory-unit drive
letter in .rdata, which the executable range happens to cover.

And the macOS line for the same run is **byte-identical**, down to both
checksums: `0xC3C92375 -> 0x29A26970`, same page, same dword, same values. Two
hosts, one behaviour, and it is not code.

**2. Structural, and already written down.** The 0x80000000 contiguous window
is not a view of the RAM mapping. `xbox_memory_layout.c` says so, and says why:

> Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
> aliases physical RAM, but we load the XBE image into the low addresses of
> that same region, so aliasing would put a title's pinned pools on top of its
> own code. Separate storage preserves that pinned-pool layout.

The ring lives at 0x80001000-0x80081000. On hardware that would alias physical
0x1000 and would indeed land on the image. Here it cannot: it is separate
storage, and this runtime made that choice deliberately, for this exact reason.

**3. The overlap was a presentation artefact.** The device fields read

    +0x24=80001000 +0x28=80081000

and the probe prints `bounds 0x00001000-0x00081000` because
`main.c:426` masks them: `MEM32(JSRF_PB_START_VA) & 0x03FFFFFFu`, converting a
CPU address to a physical offset, as its own comment says. The masked value was
then read back as a guest VA and compared against .text. The two numbers were
never in the same address space.

**Consequence.** No Windows measurement is void on these grounds. Everything in
`CLAUDE_PROGRESS_2026-09-11_TWO_FUNCTIONS_READ.md` and in the WINDOWS_BOOTS
note stands, and the plan's "DO THIS BEFORE ANYTHING ELSE" gate is passed.

The ring base is still *wrong* -- physical 0x1000 is not a plausible push
buffer address -- but that is the known symptom of D3D never allocating, not a
cause of corruption. Do not re-derive it as a new lead.

## The race is closed, and it was not the crash

`db7d9e4` publishes the slot with a release store and validates the routine
against the dispatch table in all three pumps before any other field is read.

It works: `[KERNEL] ISR 0xFFFFFF00 not in dispatch` appeared in the old Windows
log and appears **zero** times now -- the entry is rejected before dispatch
instead of after. `not_ready=0` on macOS over 40 s, which is the control saying
the counter measures the race and not something ordinary.

**It does not stop the crash.** Windows still dies the same way:

    EXCEPTION: access violation (0xC0000005)
    GUEST REGISTERS: EAX=FFFFFF00 ECX=FFFFFF00 ...

The handover's suspicion, the plan's step 2, and this fix were all reasonable
and none of them was the mechanism. That was flagged as unproven when the fix
went in and it is now measured as not proven. `0xFFFFFF00` is a value the guest
is computing and then using as a pointer; where it comes from is open, and it
is the same shape as the `0xFFFFFFB4` (-76) in the MCPX alias fault recorded in
`../goals/JSRF_GOALS_2026-09-11_ANIMATION_AND_INPUT.md` -- an error return used
as a pointer. Worth checking whether they are the same bug.

## What the instrument cost, and the trap in it

Two false alarms before it was right, both on macOS, both caught only because
it reports the changed OFFSET and VALUES rather than a checksum:

  - baselining at section load flagged page `0x001C3000` every run -- the
    kernel thunk table at `0x001C3F60`, which our own `xbox_kernel_bridge_init`
    patches after load. Our write, read back as the guest corrupting itself.
    The baseline now runs after that init.
  - the surviving one-dword change then read as confirmed corruption until the
    dword was decoded as `"MU_0"`.

A bare "page N changed" would have reported code corruption on macOS, twice,
and been believed. If this probe is ever reduced to a checksum, it becomes a
liar.

## Caveat on the Windows runs

The crash is nondeterministic in timing: one run reached `[GPU] clear colour`
and `[VSH]` before dying, another died at ~300 ms. The `[TEXT-CK]` comparison
above comes from a run that got one report in before the crash, and periodic
reports stop when the guest stops -- so the window observed is early. It is
enough for the structural argument, which does not depend on timing, but a
longer Windows window has not been checked and a run that reaches the pusher
AND reports would be worth having.

Reproduce: build `jsrf_first_fault` only (the `pb_scan_owner_test` and
`d3d8_smoke` targets do not cross-compile -- `setenv`, `nv2a_vsh.h`), stage the
exe in the `recomp-gate` bottle, drive it with `CX_BOTTLE=recomp-gate` and a
`.bat` (the CrossOver `wine` wrapper mis-parses long quoted argv), game dir
over `Z:`.
