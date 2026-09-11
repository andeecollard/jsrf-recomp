# Windows stops crashing, and puts pixels in the framebuffer

Date: 2026-09-11 (Europe/London), late. Follows the contiguous-arena fix.

## Two fixes, and where they left the host

`a0cbb2a` moved the Windows contiguous arena above the loaded image. The title
then got far enough to load media and died on an assertion in our own APU voice
processor. This note is that assertion and what came after it.

## The voice list admitted handles the hardware cannot have

`voice_process` opens with `assert(v < MCPX_HW_MAX_VOICES)` and then indexes
`g_dbg.vp.v[256]` with `v` before doing anything else. The walk that feeds it
only ever checked for the terminator:

    for (int i = 0; d->regs[current] != 0xFFFF; i++) { ... }

`NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE` is masked **0x0000FFFF** -- a
full sixteen bits -- so every value from 0x0100 to 0xFFFE passes that condition
and reaches `voice_process`. In a debug build that is the assertion; in a
release build it is an out-of-bounds write into a 256-entry array, silently.
Both our builds happen to be Debug, which is the only reason this presented as
a clean abort rather than as corruption.

Guarded, and made to **report the value rather than assume it**, because "the
guest wrote a bad handle" and "we decoded the link field wrongly" want
different fixes:

    [APU] voice list 0 entry 4: handle 0xBC60 is out of range
          (max 255, terminator 0xFFFF) -- stopping this list

0xBC60 is not an off-by-one and not a near-miss sentinel; it is garbage, and it
is consistently the FIFTH link in list 0 -- the first four entries walk fine.
That shape is worth keeping: something writes four good voice links and then a
bad one, every frame.

macOS never produces it: zero guard lines across a full run, 2.7M pusher
methods, ctest 21/27 unchanged.

## Where Windows is now

    no guest fault, no assertion, no 0xFFFFFF00
    runs 85s+ (every run before the arena fix died inside 60)
    reaches its media: Media\People\People01.dat, 1,157,120 bytes
    [FB] t=84.00 0x00314000 sum=5F06A510 nonzero=8/153600

That last line is the new one. The framebuffer had read `sum=00000000
nonzero=0/153600` on every previous Windows run; it now has content. Eight
pixels is not a picture, and the CrossOver window is still black by
construction because the guest framebuffer is never connected to it on this
host -- but the GPU model is writing where the display is pointed, which it has
never done here before.

## The standing boundary

Unchanged and still the next thing:

    [PB-NOTIFY] waiting parameter=2 raised=1 pmc=00001000 intr=00100000 fifo=00000000

A PGRAPH software method with parameter=2 is raised and never acknowledged --
PGRAPH_ERROR set, PMC's PGRAPH bit set, FIFO access still suspended. parameter=9
notifies complete normally. The guest's handler services one and not the other.

## Two candidates behind that, both cheap

  - the 0xBC60 link. A voice list going bad at a fixed position each frame is
    the kind of thing that also explains a handler not completing.
  - the ADX tick, which the log reports STUCK.

Neither has been measured. They are named here so the next session starts from
them rather than from the crash, which is closed.

---

# Correction: the notify path is healthy, and the print cap fooled me twice

I wrote above that "a PGRAPH software method with parameter=2 is raised and
never acknowledged" and that "parameter=9 notifies complete normally" while
parameter=2 does not. Both halves are wrong.

    raised (printed): 16     completed (printed): 16

`jsrf_software_method` prints its `#N` and `completed` lines only for the first
sixteen (`if (++n <= 16 || parameter == 5)`), while the `waiting` branch has its
own 2-second throttle and keeps printing forever. So the visible record is
sixteen raises and sixteen completions -- every one of them acknowledged by the
guest, parameter=9 and parameter=2 alike -- followed by `waiting` lines from a
LATER notify whose number was never printed. The notify machinery added in
`dafa70c` works for both parameters.

That is the second time tonight a capped log produced a confident wrong reading,
after `[HEAP]` at 64 lines took out the plan's founding fact. The rule already
in CLAUDE.md covers it and I did not apply it: read a counter's trigger before
trusting its value, and that includes print caps.

## What the run actually shows

    [GPU] clear #600   [GPU] draw #600      then nothing further
    [FB] sum=5F06A510 nonzero=8/153600, 9 CHANGED transitions
    [PB-ACK] 1411755 loops/s acked=353 already=2526944 not-consumed=44100

So the GPU model ran ~600 clears and ~600 draws, the framebuffer changed nine
times, and then submission stopped with one notify outstanding. Draws reaching
600 and stopping is a much better-shaped problem than the crash that preceded
it.

## The PUT-backwards lines are NOT new and NOT the blocker

Three `[PUSHER] PUT backwards to a non-base address` events, the last at the
wedge. That code is diagnostic only -- it classifies and logs and changes
nothing -- and its own comment records the condition as already known: "it is
the poll on which the parse desynchronises -- once per run, one bad header in
six and a half million dwords". It is a pre-existing macOS-side observation,
not a Windows regression. `healthy base wraps so far 0` is worth keeping
though: this ring has never once wrapped to its base address.

## Honest next steps

Nothing below has been measured; they are named so the next session does not
start by re-deriving them.

  - Why submission stops after ~600 draws with a notify outstanding. The
    pusher blocks in `jsrf_software_method` until the guest acknowledges, and
    the guest may in turn be waiting on the ring to drain -- a deadlock between
    the two would look exactly like this. Check whether the PGRAPH ISR is still
    being dispatched during the stall.
  - The 0xBC60 voice link, fifth in list 0, every frame.
  - `healthy base wraps so far 0` on a ring that has run 600 draws.
