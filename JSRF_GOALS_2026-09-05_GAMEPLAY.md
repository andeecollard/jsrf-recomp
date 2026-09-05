# JSRF goals: from D3D event wait to interactive gameplay

Updated 5 September 2026. These goals supersede the active ordering in
JSRF_GOALS_2026-09-05_PUSHBUFFER_RATE.md. Historical findings remain evidence,
not acceptance for the milestones below.

## 1. Resolve the reserve event wait (active)

Measure the path from submitted GPU notification commands to the guest event
at device+0x2440. The corrected address-contract run waits at guest 0x00191510
with the parser caught up; all 387 sampled main-thread stacks were there.
Identify the missing or incorrect operation, implement its real semantics,
and demonstrate that the wait returns because its completion is delivered.
Do not bypass the branch, force the event periodically, resynchronise the
parser, or clear stream_fault. Add focused regression coverage.

### Measured 5 September: the notification path is complete and it works

The whole chain is now traced and confirmed in a running process:

    NO_OPERATION parameter 5
      -> PGRAPH software-method trap (0xFD400704 method, 0xFD400708 data)
      -> guest ISR at 0x0019428C, which reads the index from 0xFD400708
      -> sub_00193F70(device, index): a 9-entry switch on index-1,
         table at 0x001941B4
      -> index 5 is the ONLY entry reaching 0x00194144
      -> KeSetEvent(device+0x2440), the event 0x00191510 waits on

The signaller was identified by adding the guest caller to the kernel bridge's
event trace -- the literal 0x0019D640 appears nowhere in the image, because it
is only ever computed as device+0x2440. `[EVENT-SET] object=0019D640
caller=00194155`. Its sibling 0x0019D630 is the vblank event, signalled a
thousand times a run from 0x00193E62 via sub_0018CE50; a different path.

"Parameter 5 never reaches the parser" no longer holds. After the
address-contract fix, 2,554 software methods are delivered per run, every one
raised and acknowledged, and a ring scan finds the NOP-5 packet at 0x00593F70
behind the cursor.

### So the defect is upstream of the notification

    reserve wait entries at 0x00191510 :  2
    NO_OPERATION parameter 5           :  1   (in 2,554 software methods)
    EVENT-SET on device+0x2440         :  1

The wait is entered twice. The first is satisfied end to end -- one NOP 5
submitted, delivered, dispatched as index 5, event set, wait returns. That is
the path working. The second never returns because **the guest never submits a
second notification**.

Nothing in the notification path needs implementing. What needs measuring is
the decision that inserts the NOP in the first place: the reserve distance
calculation in sub_00191530/sub_00191440. That branch reads GET, and GET is
what both changes in flight alter -- the high-CPU-alias address contract and
`g_nv2a_pusher_owns_dma_get`. Check it against both.

### Cause found and fixed: the parser executed a copy, not the ring

The notification is a **patch**. loc_001914A9 writes [esi+0x10] = 0x00040100,
[esi+0x14] = 5 into a segment the title has already published, and loc_001914CD
overwrites those same two words again if the GPU catches up before it commits
to waiting. `jsrf_pb_feed` memcpy'd up to 1 MB into a snapshot and executed
that, so a patch landing after the copy was never seen:

    [PB-PATCH-LOST] address=005DF9A4 snapshot!=5 live=5 feed=005A2588-005ED000

The ring is now executed in place. Copying was protection against the producer
overwriting the ring underneath the parser, and that is already prevented at
its source by publishing DMA_GET from the consumed cursor; reading in place is
also what the hardware does.

    reserve waits at 0x00191510 :  2 -> 6
    NO_OPERATION parameter 5    :  1 -> 5
    faults 0, bad_headers 0, ctest 18/18

So the wait now returns and the loop iterates several times instead of once.

### And the iteration that stops is not a reserve defect at all

Five iterations run the full cycle -- patch the notification, publish, wait,
receive parameter 5, return. The sixth differs in exactly one visible way:

    [PB-PATCH] ... cursor=005D05C4 ... GET=0056D02C PUT=005E7904

`cursor` and `GET` disagree, where every earlier iteration had them equal. Five
log lines earlier:

    [PUSHER] rejected jump 0EF92EF8 at 005D05C0
    seg 3186: from=0056D02C end=005E7A0C put=005E7A0C stop=2 consumed=101734

The parse ran 101,734 dwords from the ring base and stopped on a bad jump at
0x005D05C0; the cursor is 0x005D05C4, the dword after it. `stream_fault`
latched, so the feed stopped publishing DMA_GET and it froze at 0x0056D02C.
The reserve routine then computed its distance from a stale GET and waited for
space that had in fact already been consumed.

**So goal 1's remaining symptom is downstream of G26** -- the one parse
desynchronisation per run -- and not a separate defect in the reserve or
notification path. Those work. Fix the desync and this loop should continue on
its own.

The desync is not explained by in-place execution: the guest's patches change
packet *parameters* (0x40100/5 to 0x40100/0, 0x40110/0 to 0x40100/0), never
packet lengths, so they cannot shift the parse. It is the same residual
one-per-run event seen before the change.

**Acceptance is unchanged:** the wait returns because its completion is
delivered, repeatedly, with no bypass and no forced event.

## 1b. The single parse desynchronisation is now the only blocker (active)

Everything converges here. The reserve wait, the notification path, the
software-method interrupt, the address contract and the GET fence all work;
each run ends because the parser desynchronises exactly once, latches
`stream_fault`, and stops publishing GET.

Two candidates remain, and one experiment separates them. The failing window is
bounded and known -- `from=0056D02C`, 101,734 dwords, stopping at 0x005D05C0.
Keep a copy of each window as it is taken, and on a parse failure re-walk that
copy **without dispatching anything**, then compare where the second walk
stops:

- stops at the same offset -> the bytes were already like that when the window
  was taken, so this is a decoding defect at a specific packet, and bisecting
  the window finds it;
- stops later or not at all -> the ring changed underneath the parse, and the
  guilty write is what to hunt.

A non-dispatching walk is required: re-running the real parse would repeat
every method's side effects.

Do not resynchronise, skip, scan forward for a plausible header, or clear
`stream_fault` to get past this.

### Answered, and fixed: GET was published once per poll

The replay says it outright:

    [PB-REPLAY] invalid header: window 0056D000 +26920 dwords;
      live stopped at dword 1893 (0056ED94);
      replay stop=0 at dword 26920 -- DIFFERENT offset:
      the ring changed during the parse

The copy taken at the window's start parses cleanly to the end. The live parse
died 1,893 dwords in. So the bytes were fine and something overwrote them
mid-parse.

That something is the producer, and we were letting it. GET is its
back-pressure, and it was published only once the whole poll had finished. A
poll could cover most of the ring -- 26,920 dwords here -- and for all of that
time GET did not move, so the title was free to fill the ring and write over
the bytes being read.

Consumption now proceeds in bounded steps of 0x8000 bytes, comfortably above
the 8 KB largest legal packet, with GET republished after every step. Measured
over 110 seconds:

    bad headers 0, invalid 0, rejected control flow 0, faults 0
    draws       9,819 (frozen) -> 638,538 and still climbing
    triangles  32,535 (frozen) -> 3,266,664 and still climbing
    pusher     270,157,486 dwords, 250,556,913 methods
    ctest 18/18

The counters advance in every report instead of freezing at the seventh. This
closes goal 1b and substantially answers goal 2.

The framebuffer is still black, and that is now goal 3's question -- a
presentation and rendering problem, no longer a stall.

## 2. Validate sustained command consumption

The high CPU alias fixes a measured GET/ring address mismatch, but a single
110-second run that later stops submitting does not close G26. Require repeated
runs beyond the prior failure point with advancing submission and consumption,
no rejected control flow, malformed headers, or permanent stream faults.

## 3. Display frames continuously (active)

Now the only thing between the port and a picture. Commands flow continuously,
the rasteriser runs, 3.2 million triangles and 4,200 presented frames in a
110-second run -- and the framebuffer probe reads `nonzero=0/153600`.

Three surfaces are in play and they are not the same memory:

    0x005F0000   the render target the CPU executor clears and draws into
                 (pitch 1280, clip 640x480, 16bpp)
    0x0071E000   what the [FB] probe samples
    the D3D11/GL backend's own framebuffer, which pgraph draws into and
    d3d8_PresentFrame presents

Split it before diagnosing anything. `RECOMP_FB_DUMP` writes the executor's
surface to a BMP on every report, so the first question -- does the rasteriser
produce pixels at all -- is answered by looking, not by inference:

- surface has content, probe reads zero -> the copy or flip from render target
  to scanout is the defect, and the probe may simply be watching the wrong
  address;
- surface is blank too -> the rasteriser is being fed state it cannot draw
  with, and the [GPU] skip/off-surface counters and the VSH reject list say
  which;
- both blank but the GL window shows something -> the executor is not the path
  that matters on this host and the D3D11 sink is.

Triangle counts do not satisfy this milestone; a displayed image does.

### Measured: it renders, and the split is answered

The executor's surface is not blank. `RECOMP_FB_DUMP` over a 110-second run:

    frame001-003   the anti-graffiti screen, **pixel-correct** -- red spray-can
                   logo, four paragraphs of legible text, correct scale
    frame008       the title screen: real art, "FUT" and "T" readable, a circle
                   and grid rules -- but drawn about 4x oversized, so only a
                   magnified corner is on screen
    most others    black, sampled between a clear and its draws

So the rasteriser, the vertex shader, the texture path and the colour packing
all work. The `[FB]` probe reading `nonzero=0/153600` was watching 0x0071E000
while the executor draws into 0x005F0000; that probe address is a separate
question and not evidence of a blank renderer.

**Correction.** I read oPos = (0,0), (2560,0), (0,1920) as a 4x scale defect.
It is not: that is the startup full-screen blit, drawn as one oversized
triangle covering the viewport, which is a standard technique. The same
coordinates appear in the runs whose output is pixel-correct, so they are not
evidence of anything wrong. Withdrawn.

What is measured is that later screens draw recognisable art -- letters,
panels, thin rules -- at a scale that puts only a fragment of it on screen,
while the viewport the title programs is textbook for 640x480:

    viewport scale 320.000 -240.000 16777215.000 0.000
    viewport offset 320.531 240.531 0.000 0.000

So the viewport methods are being received correctly and the defect is in what
the title's own geometry is multiplied by, not in the viewport.

### 3a. CLOSED. The title screen renders correctly

There is no scale defect. Frame f035 of the composed-frame capture is JSRF's
title screen: the logo and character graphic, "JSRF" and "JET SET RADIO
FUTURE(TM)" beneath it, correctly scaled, centred and legible.

Three readings of mine were withdrawn getting here, and the pattern in all
three was inferring from a fragment:

- oPos = (0,0), (2560,0), (0,1920) is the startup blit's oversized covering
  triangle, a standard technique, and appears in frames that are pixel-correct;
- the viewport the title programs is textbook -- scale 320/-240, offset
  320.531/240.531 -- so nothing needed compensating there;
- the "oversized" frames f008 and f039 were captured mid-composition, showing
  individual UI elements before the rest of the frame was drawn. Sampling a
  drawing batch is not the same as sampling a finished frame.

The title's own geometry transforms to sensible screen coordinates:
oPos = (365.7, 192.9, 1.673e7, w=221.0) for a 16-slot program, well inside
640x480.

**`[FB] nonzero=0/153600` was never about the renderer.** The probe samples
0x0071E000; the executor draws into 0x005F0000. Point the probe at the render
target, or follow the flip, before reading anything into it again.

### 3b. Continuous display (active)

What is demonstrated is that correct frames are composed. What is not is that
they are presented continuously and visibly. Measure frame completion at the
flip rather than at a drawing batch, confirm the presented image updates
through startup and into the title screen, and fix the [FB] probe's address so
it reports the surface actually being drawn.

**Acceptance:** a sequence of captured frames showing the title screen present
and update, not a single frame.

## 4. Verify controls and reach gameplay

Connect the existing host input path, confirm guest-visible button/axis changes,
use controls to enter gameplay, and verify controllable movement in a rendered
scene. Record the tested route and remaining visual/audio/logic defects.

## Constraints and definition of done

Work in the current macOS harness; no upstream merge or full regeneration is
needed by default. Preserve existing uncommitted work and permanent fault
visibility. Run relevant regression checks for each behavioral change. The
main goal completes only after sustained displayed frames and interactive
gameplay are demonstrated, not merely after startup or another wait is fixed.
