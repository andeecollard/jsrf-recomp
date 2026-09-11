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

## Standing traps, all paid for today

- An ordered divergence is a CEILING on where the fault is, never a location,
  until arming more sites stops moving it. It moved three times.
- Collapse consecutive repeats before diffing two call sequences; a poll with a
  different spin count offsets everything after it.
- Take the last contiguous run of [FUNC-HIT] lines, never a fixed tail.
- The CrossOver window is always black by construction and is not a symptom.
- RECOMP_AC97_READY is mandatory on Windows.
- The archived gen is the as-found tree and already carries 13 probe sites.
