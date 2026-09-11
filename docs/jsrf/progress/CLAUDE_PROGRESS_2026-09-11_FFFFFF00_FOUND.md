# The 0xFFFFFF00 crash: named, located, and not what it looked like

Date: 2026-09-11 (Europe/London), late. The crash that has ended every Windows
run since the oracle booted.

## It is a guest pointer, not a Wine artefact

Earlier tonight I suggested this might be an artefact of running under Wine
rather than native Windows, and said the cheap test was to try real Windows.
That was wrong and the test is unnecessary. The host fault address translates:

    guest 0xFFFFFF00 + g_memory_offset 0x02000000 = host 0x101FFFF00
    guest 0xFFFFFF10 + g_memory_offset 0x02000000 = host 0x101FFFF10

and those are exactly the two addresses observed. The guest is dereferencing
guest VA 0xFFFFFF00 -- recompiled code using -256 as a pointer. Wine is
reporting it, not causing it.

## Where

Symbolised from the Wine backtrace (the .exe keeps its symbols; the image base
is 0x140000000, so `addr2line -e jsrf_first_fault.exe <rip>` resolves frames
directly):

    kernel_thunk_dispatch
      bridge_KeWaitForSingleObject
        bridge_timers_poll
          bridge_run_dpc            <-- a guest TIMER DPC
            sub_001C2319            XPP
              sub_001BF815
                sub_001BFC5E
                  sub_001BFB97
                    sub_001BFA3A    <-- faults

`sub_001BFA3A` walks a singly linked list whose head is the global at guest VA
**0x002648D4**:

    eax = MEM32(0x2648D4);
    if (eax == 0) { ...empty... }
    MEM32(0x2648D8) = eax;
    ecx = MEM32(eax + 0x10);   <-- faults here, eax = 0xFFFFFF00

0x002648D4 is in the BSS part of .data, so it starts at zero and something
writes it.

## What is writing it -- and the instrument that cannot see it

`RECOMP_MEM_WATCH=0x2648D0:0x10` on Windows catches eleven guest stores and
proves, by its own `old=` fields, that it is missing others:

    ... 0x2648D8 old=0x00000000 new=0x0066D1B0   (from sub_001BF72C)
    ... 0x2648D1 old=0xFF       new=0x95         (from sub_001BF6B5)
    ... 0x2648D8 old=0xFFFFFF00 new=0xFFFFFF00   (from sub_001BFA3A)

0x2648D8 went 0x0066D1B0 -> 0xFFFFFF00, and 0x2648D1 read back 0xFF, with no
store recorded for either.

**The memory watch is blind to string operations.** `rep movsd` / `rep stosd`
are lifted to `memcpy` plus raw `MEM32(...) = ...` and `_d[_i] = _s[_i]` loops
which do not go through `RECOMP_MEM_WRITE32`. Confirmed by reading the
generated code. That is a systematic coverage hole, not a one-off, and it is
almost certainly the real meaning of the standing note that mem-watch "logs
nothing, silently" on a working build -- it logs what it covers, and a block
copy is not covered.

So the corrupting write is a guest block copy or fill. The 0xFF pattern with a
zero byte at the bottom is the shape of a buffer filled with 0xFF and then
partly overwritten.

**The OHCI `memset(out, 0xFF, len)` at xbox_usb_ohci.c:304 is NOT it.** It was
the obvious suspect -- same fill byte, and XPP is the input library. Running
Windows with `RECOMP_OHCI_ATTACH` unset reproduces the corruption and the crash
byte for byte. Hypothesis killed by one run and no code change.

## The differential: same path, six times instead of once

From the 547-site armed corpus, one run per host:

    sub_001BFA3A   mac=1   win=6
    sub_001BF815   mac=1   win=6
    sub_001BF72C   mac=1   win=6
    sub_001BEA33   mac=2   win=7
    sub_001BD05D   mac=1   win=1

macOS is not avoiding this code. It runs it **once**. Windows runs it **six**
times, and the sixth finds the list head at 0xFFFFFF00.

That reframes the whole thing. The question is not "why does Windows enter an
XPP path macOS never enters" -- it enters the same path, repeatedly. The
question is **why the XPP timer DPC re-fires on Windows and fires once on
macOS**, and whether the list is simply being consumed and freed on the first
pass and walked again afterwards.

`bridge_timers_poll` is the place to look:

  - it re-arms anything with a non-zero `period_ms` and disarms one-shots, so a
    one-shot mis-recorded as periodic re-fires for ever;
  - it calls `bridge_run_dpc(dpc_va, 0, 0)` with no validation of `dpc_va`
    beyond what bridge_run_dpc does itself -- the same shape as the
    KeConnectInterrupt slot that was fixed earlier tonight;
  - whether `KeCancelTimer` actually disarms the slot wants checking. If the
    guest cancels and we keep firing, this is exactly what it would look like.

## Next

1. Log timer arm/cancel/fire with the timer VA, DPC VA and period on both
   hosts, and diff. One counter set; the asymmetry is 1 against 6, so it should
   be unmissable.
2. If Windows is re-firing a one-shot or a cancelled timer, that is the bug and
   it is in shared code -- macOS would have it latent, exactly like the four
   the oracle has already turned up.
3. Do NOT reach for the mem-watch to find the corrupting write until it covers
   string operations. Extending `RECOMP_MEM_WRITE` to the lifted `rep`
   sequences is a real change to the translator's output, and worth doing:
   every block copy in the title is currently invisible to it.

---

# Addendum, same night: the saved Wine backtrace

The full crash report (saved from Wine's dialog, kept at
`../artifacts/win_backtrace_2026-09-11_ffffff00.txt`) carries 27 frames, not
the 9 the dialog showed. Symbolised:

    main -> xbe_entry_point -> sub_00147F53 -> kernel_thunk_dispatch
      -> bridge_PsCreateSystemThreadEx -> sub_00147EBB -> sub_00147FB4
      -> sub_0006F9E0 -> sub_00013F80 -> sub_00013A80 -> sub_0014D090
      -> sub_00198F10 -> sub_00198ED0 -> sub_00198670
      -> sub_0018CE50                          <-- D3D's vblank wait
        -> kernel_thunk_dispatch -> bridge_KeWaitForSingleObject
          -> bridge_timers_poll -> bridge_run_dpc
            -> sub_001C2319 (XPP DPC) -> ... -> sub_001BFA3A   CRASH

`sub_0018CE50` is named in `kernel_bridge.c`'s own comment: it is the D3D
routine that clears the SignalState of the device's vblank KEVENT and waits on
it. So the XPP timer DPC runs **nested on the D3D thread inside its vblank
wait**, which is by design -- the wait pumps the timers.

Faulting instruction, from the report: `movl (%rax), %ebx`, rax = 0x101ffff10.

## Tested and rejected: concurrent DPCs

`g_in_dpc` is `RECOMP_TLS`, so `bridge_timers_poll`'s guard is per-thread and
does not stop two threads running guest DPCs at once, while the vblank and
device pumps do take a process-wide interlock. That looked like the answer.

Giving `bridge_timers_poll` the same interlock changes nothing: the corruption
and the crash reproduce byte for byte. **Reverted** rather than left in -- it is
an unproven change to shared code on a build that works, which is exactly what
this tree's rules say not to land. The structural observation stands and is
worth fixing on its own merits some other time; it is not this bug.

## 0xFFFFFF00 is also an indirect call target

    [ICALL] Failed to resolve VA 0xFFFFFF00 (total calls: 3133)

Three of these fire before the crash, and macOS logs **zero** occurrences of
0xFFFFFF00 anywhere. So the value is not only walked as a list pointer, it is
called through. It clears `RECOMP_ICALL_IS_CODE` (which accepts anything
>= 0xFE000000) and then fails the kernel lookup, so it reaches the dispatcher
looking like a thunk. Nothing in our own code writes it -- the guest computes
it.

## The unbridged ordinals are NOT the differential

The log warns `no bridge for ordinal 1 (slot 103), returning 0`, and warns that
a missing `stdcall_args_for_ordinal` entry corrupts the caller's stack. That
looked decisive. It is not: **both hosts hit exactly the same three** -- 46, 144
and 1 -- and only Windows produces 0xFFFFFF00. Ordinal 1 is
AvGetSavedDataAddress, and 0 - 0x100 = 0xFFFFFF00 is a tempting arithmetic
coincidence, but the ordering says otherwise: on macOS the ordinal-1 warning
lands *after* `[AV] SetDisplayMode`, on Windows the flow has already diverged.

## The live lead: the framebuffer is inside the image on Windows

Both hosts call AvSetDisplayMode with identical mode, format and pitch, and get
different framebuffers:

    macOS    [AV] SetDisplayMode mode=0x88070701 format=0x11 pitch=1280 fb=0x0071E000
    Windows  [AV] SetDisplayMode mode=0x88070701 format=0x11 pitch=1280 fb=0x001B2000

0x0071E000 is above the loaded image. **0x001B2000 is inside it** -- within
DSOUND's section (0x19E340..0x1BA83C), and a 640x480 2bpp surface from there
runs to about 0x248000, straight through MMATRIX, XGRPH, XPP (0x1BC7C0) and
.rdata.

That is the same shape as the push-buffer ring landing at physical 0x1000: on
this host the title's allocations come back at addresses that collide with its
own image. It would explain XPP's structures being overwritten with pixel data
and it would explain 0xFFFFFF00 appearing in both a list head and a vtable
slot.

**Not proven, and one measurement already argues against the simple version:**
`RECOMP_TEXT_CHECKSUM` covers 0x81000..0x244000 as its control half and saw
only the one benign .rdata dword move. If a framebuffer were being blitted
across 0x1B2000..0x248000, many of those pages would change. So either the
surface is never actually written at that address, or it is written somewhere
else than the mode call reports. Re-run TEXT-CK with the inner window aimed at
0x1B2000 (`RECOMP_TEXT_CK_WINDOW` exists for exactly this) before believing it.

Next: find who chooses that framebuffer address, and why it differs. Both hosts
ran identical D3D allocation calls earlier tonight, so this is downstream of
those and is a different allocator.
