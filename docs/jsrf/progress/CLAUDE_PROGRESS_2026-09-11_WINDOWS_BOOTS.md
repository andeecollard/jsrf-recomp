# Windows boots past DirectSound; the pusher is the next boundary

Date: 2026-09-11 (Europe/London), evening. Written after committing.

## State

The Windows oracle boots. Codex's AC97 range routing closed the DirectSound
stall that this session's write-clear implementation had left one level short,
and the result is confirmed in its log rather than taken on trust:

    vector connects   1, 3, 5, 6      (vector 5 = 0x001A2681, the success check)
    [APU] started by the title (SECTL=0000000F FECTL=0000100F)
    ADX               tick=205670     (was stuck at 0)
    APU frames        3,704,840
    vblank            151,786 over 2,469,972 ms = 61.5 Hz, 0 unacked skips
    NtOpenFile        21               (was 4)

Everything is on main, seven commits, tree clean, 19 ahead of origin/main.
Nothing is pushed.

Background: CLAUDE_HANDOVER_2026-09-11_WINDOWS_ORACLE.txt (reasoning and
retractions), CLAUDE_TO_CODEX_HANDOVER_2026-09-11_WINDOWS_ORACLE.txt (actions),
CODEX_PROGRESS_2026-09-11_WINDOWS_AC97_ORACLE.md (the fix and the D3D boundary).

## The next boundary, and why I would take it before the D3D one

Codex's note points at a D3D helper, sub_001910E0, and is careful to say the
evidence only places the Windows stop between two instrumented entries. That is
honest and it is also a hard place to make progress: the deciding code is in
uninstrumented game .text, which is 10,941 functions.

There is a cheaper lead in the same log. The pusher now RUNS on Windows and
produces almost nothing:

    WIN  [PUSHER] runs=14  dwords=15360  methods=24  unhandled=24
                  bad_headers=1 | clears=0 flips=0 draws=0
                  put=0x800109A4 limit=0x8001872C idx put=63 get=59
    MAC  [PUSHER] runs=31703 dwords=216,183,508 methods=200,422,656
                  clears=12428 flips=3798

15,360 dwords consumed but only 24 methods recognised, and a bad header. That
is not "the GPU is idle", it is "the ring is being parsed and the contents do
not make sense". A bad header is a specific, findable condition with a single
producer, and unlike the D3D boundary it does not need the game band armed.

Worth checking first, in order:

1. What increments bad_headers, and what it saw. One counter, one site.
2. Whether put/limit are the same shape on both hosts. Windows shows
   put=0x800109A4 limit=0x8001872C against macOS's put=0x805D1050 -- both are
   in the 0x80000000 contiguous window, but the Windows ring is an order of
   magnitude lower and much shorter, which may mean a different buffer entirely.
3. Whether the 24 unhandled methods are the same 24 macOS handles early.

## What the pusher lead turned into (measured, same evening)

Followed it. The chain below is consistent and ties to Codex's D3D boundary,
but the last link is NOT established -- see the caveat.

FROZEN, NOT SLOW. The Windows pusher line is byte-identical across all 281
reports of a ~41 minute run:

    runs=14 dwords=15360 methods=24 unhandled=24 bad_headers=1
    put=0x800109A4 limit=0x8001872C idx put=63 get=59

macOS advances every report (put 0x805D4304 -> 0x80595234, runs 937 -> 1964,
bad_headers=0). So this is not a slower GPU, it is a ring that stopped.

THE RING IS IN THE WRONG PLACE. The contiguous window at 0x80000000 aliases
physical RAM, so Windows' put resolves to physical 0x000109A4 -- inside the XBE
IMAGE, which loads at 0x00010000. macOS's resolves to 0x005D1050, in the heap
(0x00510000-0x04000000) where a pushbuffer belongs. put/limit are read from the
guest's own device fields, so 0x800109A4 is what the GUEST believes its
pushbuffer is, not something the harness invented.

WHAT THE PARSER SAW. The 24 "methods" are a contiguous ascending run --
0x1F38, 0x1F3C, 0x1F40 ... 0x1F84, each exactly once. That is what an
increasing-method command with a large count looks like when the count and
method come from data rather than a real header: the parser walks sequential
dwords, emits 24 bogus methods, then hits something that decodes as neither
method form and stops (bad_headers has one producer, nv2a_pusher.c ~158).

WHY THE GUEST THEN WAITS. GET freezes 4 behind PUT (idx put=63 get=59). Codex
independently found the guest's last instrumented entry is sub_001910E0, which
it described as computing a distance from device and push-buffer fields -- that
is PUT minus GET. A guest asking "how much ring space is free", against a GET
our pusher will never advance because it stopped on a bad header, waits
forever.

CAVEAT, and it matters: a frozen pusher is ALSO what you would see if the guest
had stalled first for an unrelated reason and simply stopped submitting. The
ordering has not been established. What argues for cause over consequence is
bad_headers=1 and the ring's location, neither of which a merely-idle guest
explains. What would settle it: whether PUT ever held a sane heap address on
Windows before 0x800109A4, i.e. whether the guest allocated a pushbuffer at all
or was handed that value from the start.

A RED HERRING, recorded so it is not re-run: xbox_MmAllocateContiguousMemory
returns VirtualAlloc(NULL,...), a HOST pointer rather than a guest VA in the
contiguous window -- which looks like the bug until you check win32_compat.c,
where the POSIX VirtualAlloc is also a plain mmap returning a host address. The
two hosts do the same wrong-looking thing, so it is not the divergence.

## CORRECTION, and what the cursor watch actually found

The section above says "the ring is in the XBE image and frozen". BOTH HALVES
ARE WRONG, and a watch on the cursor (RECOMP_PB_PUT_WATCH -- read-only poller,
no gen change) says why.

NOT IN THE IMAGE. Physical 0x1000 is BELOW the XBE, which loads at 0x00010000.
I wrote the classifier in the probe with the image base wrong and then believed
its label. The ring lives in the low 64 KB.

NOT FROZEN FROM THE START. PUT initialises to 0, becomes 0x80001000, and then
advances perfectly normally -- 0x1234, 0x1244, 0x128C, 0x1380 ... 0x1C9C across
the first 229 samples. It is a real ring that the guest walks. And in a short
run of my own the pusher keeps up completely: runs=1 methods=1011
bad_headers=0, with idx put=9 get=9 -- GET CAUGHT UP. Codex's frozen
put=63/get=59 with bad_headers=1 is from a 41-minute run, so the bad header
appears LATER, which strengthens the caveat already recorded above: the frozen
pusher is looking more like a consequence than a cause.

WHAT IS REAL, AND IS NEW. The ring occupies physical 0x1000..0x8DFC. The
harness puts its fake TIB at VA 0x1000 -- XBOX_FS_BASE, 0x30 bytes, and the
boot log says so in plain text. Those are the same memory. Worse, fs:[0] is the
SEH chain head and kernel_bridge.c writes it during ordinary operation:

    BRIDGE_MEM32(g_fs_base) = next;          (~3695)
    BRIDGE_MEM32(g_fs_base) = target_frame;  (~3726)

So on this host every SEH frame push and every RtlUnwind writes a guest pointer
into dword 0 of the push-buffer ring. That is a concrete corruption mechanism
for exactly the symptom Codex saw, and it does not exist on macOS, where the
guest's ring is at physical 0x5D4304 in the heap.

THE OPEN QUESTION IS NOW SHARPER: why does the guest's ring land at 0x1000 here
and in the heap on macOS? Both hosts return a HOST pointer from
MmAllocateContiguousMemory (see the red herring below), so neither is using it
for this. Find what actually hands D3D its push-buffer base and why the two
hosts answer differently. That is the next thing to read, and it is one
allocation site rather than a bisection.

## ROOT CAUSE: the guest never allocates a push-buffer ring on Windows

Measured, both hosts, same gen, one line each:

    MAC  [HEAP] #7: size=524288 align=4096 -> 0x0056D000..0x005ED000
         [PUSHER] ring cursor 0x0056D000  bounds 0x0056D000-0x005ED000

    WIN  requests for size=524288: ZERO, in any run
         [PUSHER] ring cursor 0x00001000  bounds 0x00001000-0x00081000

Both hosts make exactly 32 heap allocations. macOS's #7 IS the ring; Windows'
#7 is 96 bytes, so the sequence diverges before it and the 512 KB request is
never made at all. With no ring, the pusher falls back to bounds
0x1000-0x81000, and 0x1000 is XBOX_FS_BASE -- the fake TIB.

That chains to everything else already recorded here. The guest submits into
memory that is not a ring; kernel_bridge writes the SEH chain head to fs:[0],
i.e. dword 0 of that range, on every frame push and RtlUnwind; the pusher
eventually decodes data as a command, increments bad_headers and stops; GET
freezes behind PUT; and the guest waits on ring space that never frees. The TIB
collision recorded above is real but is a CONSEQUENCE of the missing
allocation, not an allocator placing the ring badly.

NEXT, and it is narrow: the heap sequence diverges before allocation #7, so
something in the preceding six returns a different answer on Windows and the
guest skips the ring allocation. Diff the two [HEAP] streams from #1 and find
the first request that differs in size or that one host makes and the other
does not. Both logs are already in the scratchpad.

## The divergence, narrowed to one allocation -- and four hypotheses killed

The heap streams are identical for five allocations and split at the sixth:

    #5  both   size=327680 align=4096
    #6  MAC    size=96      align=4096   then 524288, 622592, 1228800, 1228800
        WIN    size=180     align=16     -- and straight on with small objects

macOS makes five large page/16K-aligned GPU allocations there that Windows
never requests. Windows' #6 onward matches macOS's #11 onward, so it is not a
reordering: the whole GPU block is skipped. The align=16384 ones are the
signature of MmAllocateContiguousMemoryEx, and the call counts confirm it:

    ordinal 166 (MmAllocateContiguousMemoryEx)   macOS 59 calls,  Windows 0

So the question is not "where does the ring come from" but "why does D3D skip
its GPU memory setup entirely on this host", and the branch is immediately
after allocation #5.

FOUR HYPOTHESES TESTED AND DEAD. Each was plausible; each took one run:

  - RECOMP_IRQ_THREAD delivering vblanks during D3D init. Windows takes ISRs
    and DPCs in exactly that window and macOS does not, which looked damning.
    Run with the thread OFF: byte-identical, #6 still 180/16, no 524288. My
    own instrument is not the cause.
  - KeQueryInterruptTime. The two hosts' kernel import tables differ by exactly
    ONE ordinal -- 125 is bridged on Windows and a stub on macOS, so macOS's
    caller sees a clock that never moves, and a real clock is what would make a
    timeout fire. Ran Windows with the clock frozen to match: no change.
  - MmAllocateContiguousMemory returning a host pointer. True, and true on
    BOTH hosts, so not a divergence.
  - Unbridged ordinal 46 (HalReadWritePCISpace) appearing in the window.
    Called twice on both hosts.

Aside from ordinal 125, the unbridged-thunk lists are otherwise identical, so
the import surface is not where this lives.

NEXT: instrument the guest side of the branch rather than the kernel side. The
decision is made between the return of allocation #5 and the next allocation,
in D3D code that is already armed -- the D3D band was instrumented today. A
per-thread [FUNC-SEQ] capture bracketed to that window should name the function
that tests something and gives up.

## Two more of my own claims, retracted

Recorded because this file has now carried each of them as fact:

  - "The ring is in the XBE image." Wrong; physical 0x1000 is BELOW the image,
    which loads at 0x00010000. Corrected in commit a7870e5.
  - "NtAllocateVirtualMemory #15, 32 KB, is the ring." Wrong. Both hosts place
    that one in the heap -- 0x611000 on Windows, 0x511000 on macOS, differing
    by exactly the 1 MB worker-stack pool that XBOX_WORKER_STACK_COUNT=4 adds
    to the Windows build. The ring is 512 KB, not 32 KB.

The pattern across the whole day is worth stating plainly: every one of these
was a plausible mechanism believed before the measurement that would have
falsified it existed. The measurements were cheap in every case.

## The device fields are read correctly, and the ring base is a zero result

docs/technical/d3d-translation.md maps the D3D8LTCG device with push-buffer
BASE at +0x08 and SIZE at +0x0C, where the harness reads ring bounds from
+0x24/+0x28 "per the BO3 map". That looked like the diagnostic reading the
wrong fields, which would have made every statement here about where the ring
lives an artefact. Printed side by side on one line, both hosts:

    MAC  +0x00=8056D000 +0x04=80574DFC +0x08=0 +0x0C=0 | +0x24=8056D000 +0x28=805ED000
    WIN  +0x00=80001000 +0x04=80008DFC +0x08=0 +0x0C=0 | +0x24=80001000 +0x28=80081000

+0x08 and +0x0C are ZERO on both, so that part of the doc describes Burnout 3's
D3D8LTCG build and not this title's. The +0x24/+0x28 pair holds coherent values
on both hosts and the cursor sits inside them, which is why the probe's own
validity check accepts them. THE PROBE IS RIGHT and the ring-location findings
above stand.

What the comparison does add is the shape of the failure. On Windows
0x81000 - 0x1000 = 0x80000 = 524288 exactly: the guest asked for a 512 KB ring,
computed end = base + 512 KB, and ended up with BASE 0x1000. No 512 KB heap
allocation is ever made on this host, and ordinal 166 is called 59 times on
macOS and zero times here. A base of 0x1000 is what an allocator returning
NULL/zero looks like after the title adds its own small offset -- so the
question is not "who put the ring at the TIB" but "which allocation returns
zero, and why is the failure not checked".

Clues in the tree worth acting on, in order:

  1. d3d-translation.md:456 -- "Any function that spin-waits on GPU registers
     must be stubbed entirely, not just the register read. Allocating the page
     via VEH prevents the crash but the loop spins forever on zero." That is
     exactly sub_001912A0 (the PFB flush kick and spin) and exactly what a
     VEH-only approach does. This host is VEH-only for NV2A.
  2. The device context is a STATIC object in the D3D section, not heap
     allocated, and the doc recommends capturing it from xemu. That gives a
     known-good field map instead of inferring offsets from another title.
  3. Find the allocation that returns zero. It is not MmAllocateContiguousMemory
     (host pointer on both hosts) and not MmAllocateContiguousMemoryEx (never
     reached). Something earlier in D3D's GPU setup fails and is not checked.

## Standing traps, all paid for today

- An ordered divergence is a CEILING on where the fault is, never a location,
  until arming more sites stops moving it. It moved three times.
- Collapse consecutive repeats before diffing two call sequences; a poll with a
  different spin count offsets everything after it.
- Take the last contiguous run of [FUNC-HIT] lines, never a fixed tail.
- The CrossOver window is always black by construction and is not a symptom.
- RECOMP_AC97_READY is mandatory on Windows.
- The archived gen is the as-found tree and already carries 13 probe sites.
