# Windows sees the software methods and drops them. The ring is not garbage.

Date: 2026-09-11 (Europe/London), night. The free measurement proposed in
`CLAUDE_PROGRESS_2026-09-11_WHAT_MACOS_DOES.md`, taken on both hosts.
`RECOMP_PB_NOTIFY_TRACE=1` prints `[PB-NOP]` for every method 0x100 the parser
decodes, whether or not a handler is installed, so it separates "the parser
never sees one" from "it sees one and nothing happens".

## Result

    macOS    [PB-NOP]    subch=0 parameter=00000009   (sample capped at 16)
             [PB-NOTIFY] #1 parameter=9 raised=1
             [PB-NOTIFY] completed parameter=9
             [PUSHER] methods=2,836,717 clears=7825 flips=1960 bad_headers=0

    Windows  [PB-NOP]    subch=0 parameter=00000009   x6
             [PB-NOP]    subch=0 parameter=00000002   x1
             [PB-NOTIFY] (none -- 0 lines)

**The Windows parser decodes real software methods, with the same method, the
same subchannel and the same parameter as macOS.** Then nothing happens,
because no handler is installed on that host.

## This reorders the two candidates, and corrects yesterday's framing

`CLAUDE_PROGRESS_2026-09-11_WHAT_MACOS_DOES.md` described the Windows pusher as
"a parser reading memory the producer never writes to", on the strength of the
missing physical heap alias. That is now too strong. A parser reading unwritten
memory does not decode method 0x100 subchannel 0 parameter 9 and then parameter
2 -- those are the title's own commands, in order. Whatever else is wrong with
the Windows ring, it carries genuine work.

So the ordering is the other way round from what that note implied:

  - **Item 2 (the missing software-method handler) is LIVE and is not blocked
    by item 1.** Windows reaches the notify and discards it.
  - **Item 1 (the physical heap alias) is not disproved**, but it can no longer
    be credited with the parser seeing nothing, because the parser sees
    something correct. Its status is open.

On macOS the handler does two things the Windows build does neither of: it
raises the PGRAPH interrupt so the guest's ISR runs, and it BLOCKS the parser
until the guest acknowledges -- "The parser must not execute a later software
method until this one is acknowledged." Windows applies neither the signal nor
the ordering.

That is the strongest available candidate for "the guest stops driving D3D":
the title issues a notify and waits for an interrupt that is never raised.
Still a candidate. Not demonstrated, because nothing here shows the guest
waiting on it -- that needs the fix, or a probe on whatever the title does next.

## DO NOT just delete the #if

The obvious-looking change is wrong and will hang the pusher thread:

    #if !defined(_WIN32) && defined(__aarch64__)
        nv2a_pusher_set_software_method_handler(jsrf_software_method);
    #endif

`jsrf_software_method` calls `xbox_Nv2aRaiseSoftwareMethod`, which on Windows is
a hardcoded `return FALSE`, and then spins:

    while ((!raised || xbox_Nv2aSoftwareMethodPending()) && !g_pushbuf_ack_stop)

`raised` is FALSE forever, so the loop never exits. Installing the handler
without implementing the raise converts a dropped notify into a wedged parser.

## What the Windows implementation actually needs

The macOS raise is not a flag; it models the PGRAPH trap the way hardware
presents it, and it needs a guarded page to do it:

    regs[0x400704/4] = (subchannel << 16) | 0x100;  /* TRAPPED_ADDR   */
    regs[0x400708/4] = parameter;                   /* TRAPPED_DATA   */
    regs[0x400108/4] = 1;                           /* NSOURCE        */
    regs[0x400720/4] = 0;        /* suspend until the guest restores FIFO */
    regs[0x400100/4] |= PGRAPH_ERROR;
    PMC_INTR_0 |= PMC_INTR_PGRAPH;

and `Pending` reads 0x400100 / 0x400708 / 0x400720 back to see whether the
guest has acknowledged. All of it is gated on `g_nv2a_pgraph_guarded`, because
the acknowledge is a guest WRITE that has to be observed.

So the Windows work is three parts, in order:

1. Guard the PGRAPH page in the Windows VEH, alongside the PCRTC_INTR_0 and
   AC97 pages that already are. **Emulate the acknowledge inside the handler.**
   CLAUDE.md names PGRAPH_INTR specifically as still having the shape that cost
   this tree 13.5M lost writes -- a thread that unprotect/store/reprotects that
   page opens a window where guest stores vanish into RAM. The macOS path can
   use VirtualProtect because it is inside its own trap under `mcpx_lock`; the
   Windows path must not copy that shape from outside the handler.
2. Implement `xbox_Nv2aRaiseSoftwareMethod` and `xbox_Nv2aSoftwareMethodPending`
   for Windows against that page, replacing the `return FALSE` / `return 0`
   stubs.
3. Only then install the handler, and only with the spin loop's timeout path
   already proven -- `[PB-NOTIFY] waiting ...` exists and prints PMC, PGRAPH
   INTR and FIFO access every 2 s, which is the instrument for a raise that is
   never acknowledged.

`kernel_bridge.c`'s PGRAPH ISR loop is gated on the same `Pending` stub, so it
comes alive with step 2 and wants watching when it does.

## Run conditions

Both runs: same tree, same gen, `RECOMP_PB_EXEC=1 RECOMP_OHCI_ATTACH=1`.
Windows additionally `RECOMP_AC97_READY=1 RECOMP_IRQ_THREAD=1`. The Windows run
faulted again at `sub_00147EBB` with the unexplained 0xFFFFFF00, before any
`[PUSHER] runs=` report, so no pusher totals for that host this time -- the
`[PB-NOP]` lines land before the fault.

Both print caps matter and neither count is a total: `[PB-NOP]` stops at 16 and
`[PB-NOTIFY]` at 16. Read these as presence and absence, not magnitude. (The
`[HEAP]` cap took out the plan's founding fact earlier tonight for exactly this
reason.)
