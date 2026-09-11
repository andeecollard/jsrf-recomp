# Plan: getting graphics up on the Windows oracle

Date: 2026-09-11, written at the end of the session that built the oracle.
Background: `../progress/CLAUDE_PROGRESS_2026-09-11_WINDOWS_BOOTS.md` and the
two handovers of the same date.

## RETRACTED, 2026-09-11 night -- READ THIS BEFORE THE REST OF THE FILE

The fact this plan is built on is false, and most of what follows from it is
dead. `MmAllocateContiguousMemoryEx` IS called on Windows and DOES return
memory; the "zero times" was a count of `[HEAP]` log lines against a cap of 64
(`heap_trace_limit()`), not a count of calls. Measured directly, the two
allocators behave IDENTICALLY on both hosts -- same sizes, same descriptors,
same thunk, zero failures either side. See
`../progress/CLAUDE_PROGRESS_2026-09-11_ALLOCATORS_EXONERATED.md`.

What survives: Windows ENTERS these functions 3 and 2 times where macOS enters
221 and 358. Everything inside them is identical. The fault is upstream of the
D3D allocation path entirely, and the live question is what stops the guest
driving D3D at all -- likely the same question as the unexplained 0xFFFFFF00
crash, now that both the interrupt race and the .text overlap are eliminated.

Steps 1 and 2 below were done and are still worth having. Steps 3-5 are
superseded.

## The state in three lines (AS ORIGINALLY WRITTEN -- the last sentence is wrong)

Windows boots, runs the APU, connects all four interrupts, delivers vblank at
62 Hz and streams ADX. It renders nothing. The one fact that has survived every
test: `MmAllocateContiguousMemoryEx` (ordinal 166) is called 59 times on macOS
and zero times on Windows, so D3D never allocates GPU memory and the push
buffer falls back to a bogus base.

## Scale correction (2026-09-11)

"Windows walks in a handful of times" is true of these two functions and false
of the machine. Normalised against the 547 sites armed on both hosts, Windows
runs the interrupt and poll path five to six times MORE than macOS. It is not
idle or short; it spins. Read every Windows deficit below against that
baseline, not against an assumption that the host is doing less.

## The target is now two functions

The `[HEAP]` lines carry the guest return address of each allocation, and on
macOS every ordinal-166 call comes from one of two sites -- `ra=0x0018E6E9` and
`ra=0x00199789`, inside `sub_0018E670` and `sub_00199760`, both in the D3D
section. Both are armed, and both ARE entered on Windows:

        sub_0018E670    mac=235    win=15
        sub_00199760    mac=979    win=77

(Counts corrected 2026-09-11 to one run per host, both read from the last
contiguous [FUNC-HIT] table in the same logs the allocations were counted in.
The earlier win=3/win=2 came from a shorter run and understated entry.)

"So the branch is INSIDE these two, not upstream of them" -- WITHDRAWN. macOS
allocates on 2.6% and 5.4% of entries, so Windows' 92 entries would be expected
to yield 4.6 allocations and yielded none: P = 0.009, one run, and the
sub_0018E670 half of it is worthless alone (P = 0.68). That is suggestive of an
inside-branch difference, not evidence of one. The larger and far more certain
effect is the entry deficit itself -- 92 against 1,214, and ~70-90x once
normalised by the scale correction above -- which is upstream of both.

## Steps, in order, with what each settles

1. SETTLE THE .text QUESTION FIRST. -- DONE AND CLOSED. The guest is NOT
   overwriting its own code. Zero of 8 pages inside the ring window change on
   Windows, the one control page that does is byte-identical to macOS ("MU_0"
   -> "MU_7" in .rdata), the 0x80000000 contiguous window is deliberately not a
   view of the RAM mapping so the ring cannot reach .text, and the "overlap"
   came from the probe printing device field 0x80001000 masked to 0x00001000
   and comparing it to a guest VA. No Windows measurement is void on these
   grounds. See ../progress/CLAUDE_PROGRESS_2026-09-11_TEXT_QUESTION_SETTLED.md.
   Original text follows. RECOMP_TEXT_CHECKSUM=1 sums 16 code pages, 8 inside the ring
   window and 8 outside as a control, and names the changed offset and values
   rather than only the checksum. On macOS: nothing inside the window moves,
   and the single control page that does is one dword at 0x001C3F20 going
   "MU_0" -> "MU_7", a drive letter in .rdata. That is the baseline to compare
   the Windows run against. Original text follows.

    The Windows ring bounds (0x1000-0x81000)
   overlap the title's .text (0x11000-0x18CB30) by 448 KB. If the guest is
   writing command words over its own code, every later measurement is
   untrustworthy and that is the whole story. Checksum one .text page at
   startup and again in the periodic report; if it changes, stop and fix that.
   ~20 lines, one run per host. DO THIS BEFORE ANYTHING ELSE.

2. FIX THE RECOMP_IRQ_THREAD RACE. -- DONE, db7d9e4, AND IT WAS NOT THE CRASH.
   The validator demonstrably works ("ISR 0xFFFFFF00 not in dispatch" is gone),
   and Windows still dies with EAX=ECX=0xFFFFFF00. Open question, now with the
   .text explanation eliminated as well: where does the guest get 0xFFFFFF00
   and use it as a pointer? Same shape as the -76 in the MCPX alias fault.
   Original text follows. Release store on publish,
   acquire load plus routine-against-dispatch-table validation in all three
   pumps. not_ready= on the [VBLANK] line counts rejections and reads 0 on
   macOS, which is the control. NOT proven to be the Windows crash mechanism --
   the .text question above explains it equally well. Original text follows.

    It is mine: the guest publishes
   g_interrupts[i] and then fills the KINTERRUPT, unsynchronised, which was
   safe only while the poll ran on the guest's own thread. It kills Windows
   runs before they reach a report, which blocks step 3. Publish the slot only
   once the structure is complete, or validate the routine before reading any
   of it. ~10 lines.

3. READ THE TWO FUNCTIONS. -- DONE, 2026-09-11. See
   `../progress/CLAUDE_PROGRESS_2026-09-11_TWO_FUNCTIONS_READ.md`. Neither
   contains a capability check or a mode field; each has exactly one branch
   before the allocation, and it is the same call in both --
   `sub_0014A83E`, a wrapper over the title's heap. The allocation itself is
   an indirect call through kernel thunk slot 102, confirmed from the XBE as
   ordinal 166. The icall is not failing to resolve: the only ICALL failure on
   Windows is a single null target that macOS also has exactly once.

4. ARM `sub_0014A83E` -- replaces the argument-instrumentation step, which the
   counts do not support. Windows enters the two functions 15 and 77 times
   against macOS's 235 and 979, and macOS allocates on only 2.6% and 5.4% of
   entries, so 92 Windows calls cannot resolve a branch at that rate (P = 0.009
   for the whole observation, from one run). `sub_0014A83E` is armed in no run
   in the corpus and is the only untested link in the chain; it separates "the
   guest never reaches the allocation" from "the allocation returns zero" with
   one site. Arm `0x0014A83E` and `0x001497DC`, and log `MEM32(0x27DCD4)` --
   the title's heap handle -- once at startup.

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

## Retired

  - the ring scribbling over the title's .text: measured on both hosts, and
    structurally impossible in this runtime. Closed, see step 1.
  - the KeConnectInterrupt race as the Windows crash mechanism: fixed, and the
    crash is unchanged.
  - the D3D contiguous allocation failing on Windows: it does not fail. The
    two allocators, their heap descriptor, and thunk slot 102 are all measured
    identical on both hosts. Do not re-open any of them.
  - any conclusion drawn by counting [HEAP] lines: the log is capped at 64 and
    says so nowhere. Raise it with RECOMP_HEAP_TRACE=<n> before counting.

  - instrumenting the two functions' arguments (step 4 as originally written):
    the event rate is 2.6-5.4% and Windows supplies 92 samples.
  - the icall through thunk slot 102 failing to resolve: measured on both
    hosts, one null target each, not a discriminator.
  - the 187 sites present on macOS and absent from Windows as a lead: macOS
    reached gameplay and Windows has not, so they are absent by construction.
