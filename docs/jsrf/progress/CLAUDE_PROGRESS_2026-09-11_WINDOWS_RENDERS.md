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
