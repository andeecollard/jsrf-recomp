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

## Standing traps, all paid for today

- An ordered divergence is a CEILING on where the fault is, never a location,
  until arming more sites stops moving it. It moved three times.
- Collapse consecutive repeats before diffing two call sequences; a poll with a
  different spin count offsets everything after it.
- Take the last contiguous run of [FUNC-HIT] lines, never a fixed tail.
- The CrossOver window is always black by construction and is not a symptom.
- RECOMP_AC97_READY is mandatory on Windows.
- The archived gen is the as-found tree and already carries 13 probe sites.
