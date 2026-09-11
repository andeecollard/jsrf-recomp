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
