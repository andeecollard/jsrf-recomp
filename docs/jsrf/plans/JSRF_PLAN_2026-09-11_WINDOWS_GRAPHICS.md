# Plan: getting graphics up on the Windows oracle

Date: 2026-09-11, written at the end of the session that built the oracle.
Background: `../progress/CLAUDE_PROGRESS_2026-09-11_WINDOWS_BOOTS.md` and the
two handovers of the same date.

## The state in three lines

Windows boots, runs the APU, connects all four interrupts, delivers vblank at
62 Hz and streams ADX. It renders nothing. The one fact that has survived every
test: `MmAllocateContiguousMemoryEx` (ordinal 166) is called 59 times on macOS
and zero times on Windows, so D3D never allocates GPU memory and the push
buffer falls back to a bogus base.

## The target is now two functions

The `[HEAP]` lines carry the guest return address of each allocation, and on
macOS every ordinal-166 call comes from one of two sites -- `ra=0x0018E6E9` and
`ra=0x00199789`, inside `sub_0018E670` and `sub_00199760`, both in the D3D
section. Both are armed, and both ARE entered on Windows:

        sub_0018E670    mac=235    win=3
        sub_00199760    mac=895    win=2

So the branch is INSIDE these two, not upstream of them. Windows walks in a
handful of times, returns without allocating, and never comes back. That is a
far smaller target than anything chased today.

## Steps, in order, with what each settles

1. SETTLE THE .text QUESTION FIRST. The Windows ring bounds (0x1000-0x81000)
   overlap the title's .text (0x11000-0x18CB30) by 448 KB. If the guest is
   writing command words over its own code, every later measurement is
   untrustworthy and that is the whole story. Checksum one .text page at
   startup and again in the periodic report; if it changes, stop and fix that.
   ~20 lines, one run per host. DO THIS BEFORE ANYTHING ELSE.

2. FIX THE RECOMP_IRQ_THREAD RACE. It is mine: the guest publishes
   g_interrupts[i] and then fills the KINTERRUPT, unsynchronised, which was
   safe only while the poll ran on the guest's own thread. It kills Windows
   runs before they reach a report, which blocks step 3. Publish the slot only
   once the structure is complete, or validate the routine before reading any
   of it. ~10 lines.

3. READ THE TWO FUNCTIONS. sub_0018E670 is 0xBB bytes and sub_00199760 is 0x56
   -- both small. Read what they test before the allocation call. A capability
   check, a null from an earlier call, or a mode field are all plausible and
   the code will say which.

4. IF READING DOES NOT SETTLE IT, instrument their arguments rather than more
   functions. instrument_func_hit.py already supports --stackarg, --ecxfield
   and --ecxpair, so the `this` pointer and the size/alignment arguments can be
   recorded on both hosts and diffed. That is a direct comparison of what the
   guest asks for, not another bisection.

5. ONLY THEN widen tracing, and if you do, fix the sequence ring first: thread
   slots are handed out by first-touch order, so cross-host diffs are invalid
   until rings are keyed by a stable identity. The `first=` field added today
   is the start of that and has not been exercised.

## Do not re-run these

Nine hypotheses died under measurement today. Each looked convincing:

  - RECOMP_IRQ_THREAD causing it (off: byte-identical)
  - KeQueryInterruptTime, the single ordinal the import tables differ by
    (frozen to match macOS: no change)
  - MmAllocateContiguousMemory returning a host pointer (it does -- on BOTH)
  - unbridged ordinal 46 / HalReadWritePCISpace (called on both)
  - the D3D8 HLE probe (enabled on Windows: no change)
  - AvSetDisplayMode (downstream of the allocations, not upstream)
  - the push-buffer probe reading the wrong device fields (+0x08/+0x0C are
    zero on BOTH hosts; the +0x24/+0x28 pair is correct for this title)
  - the NV2A ack thread not running or targeting the wrong memory (it runs on
    both, and g_nv2a_memory is mapped at the guest's own NV2A VA)
  - NtAllocateVirtualMemory #15 being the ring (it is 32 KB; the ring is 512 KB)

## Two standing cautions

An ordered divergence is a CEILING on where a fault is, never a location, until
arming more sites stops moving it -- today's moved three times.

And read a counter's trigger before trusting it. The Windows RECOMP_APU_TRACE
prints the value read BACK from the model rather than the value written, which
made correct writes look like zeros and produced a whole wrong narrative.
