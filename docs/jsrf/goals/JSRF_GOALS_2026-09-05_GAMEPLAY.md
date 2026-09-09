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

### Measured: the probe was reading a heap block

`RECOMP_GUARD_PAGE=0x0071E000` named the writer of the address `[FB]` was
summing: **xbox_HeapAlloc**, zeroing it as an ordinary allocation. JSRF never
programs a scanout, so PCRTC_START held whatever was there, and every
`nonzero=0/153600` line in this port's history was reading heap memory rather
than a framebuffer.

The probe now reports the executor's own render target, and the answer changes
completely:

    [FB] 0x005F0000 sum=679424FD nonzero=16779/153600 CHANGED
    [FB] 0x005F0000 sum=859B5783 nonzero=2/153600     CHANGED
    [FB] 0x005F0000 sum=88880000 nonzero=2/153600     CHANGED

The surface changes between samples, with content. Zeros are samples that land
between a clear and its draws, which is what a once-a-second probe against a
60 Hz clear does.

On this host the window is fed from that same surface --
`xbox_D3D8SetGuestFramebufferSource(nv2a_pb_exec_surface)` -- so what is
composed is what is displayed.

### Measured at the flip: presentation was landing after the clear

`RECOMP_FB_DUMP_FLIP` captures at the only moment a frame is finished. The
first measurement was blunt: of 46 captures, **43 were blank**, while captures
taken mid-draw were full of content. Presentation was running after the parser
had already walked into the next frame's clear.

FLIP_STALL is the guest saying the frame is finished, and consumption now stops
on it. Same capture again:

    blank flips   43/46 -> 27/46
    with content   3/46 -> 19/46
    distinct        16   -> 19
    throughput    270M dwords -> 260M, 3,900 frames presented (unchanged)

So better, and not solved: 27 of 46 presents still carry a blank surface. The
remaining cases are worth separating before more code -- whether those flips
are genuinely blank frames the title intended, or the boundary is still being
crossed by something other than the step loop.

### 3c. Are the remaining blank presents intended? (active)

27 of 46 presents still carry a blank surface. Two possibilities, needing
opposite responses, and one number separates them: **how many triangles were
drawn between the previous flip and this one.**

- a blank present after a frame that drew thousands of triangles means the
  boundary is still being crossed and the clear is wiping composed work;
- a blank present after a frame that drew nothing means the title genuinely
  submitted an empty frame, which JSRF's mostly-black title screen may well do,
  and there is nothing to fix.

Log the triangle count at each flip and correlate it with whether that flip's
capture is blank. Do not add heuristics to skip "empty" flips or to defer
presentation until content appears; either the boundary is right or it is not.

**Answered: they are not intended.** Across the correlated captures:

    blank, and >50 triangles drawn since the previous capture : 11
    blank, and <=50 triangles                                 :  0
    with content                                              : 15

Every blank present follows a frame that drew -- around 750 triangles per
capture interval, consistently, with no empty frames at all. So the title never
submits a blank frame and the boundary is still being crossed: work is
composed, then cleared, then presented.

Stopping on FLIP_STALL was necessary and is not sufficient. The next question
is what the guest emits between its last draw and its flip -- in particular
whether the clear for frame N+1 precedes the FLIP_STALL for frame N in this
title's command order, which would make "stop at the flip" the wrong boundary
rather than a misapplied one. Dumping the method sequence around a flip
answers it.

**Acceptance:** a sequence of captured frames showing the title screen present
and update, not a single frame.

### 3c CLOSED. The presents were never blank; the capture was of the wrong buffer

Two instrument defects, and between them they produced the "27 of 46" figure.

**One: the capture sampled the live surface, not the presented one.** The window
is fed `nv2a_pb_exec_surface`, which returns `s_snap` -- the copy
`snapshot_surface` takes inside the FLIP_STALL dispatch. `RECOMP_FB_DUMP_FLIP`
called `dump_surface_bmp`, which reads guest memory at `s_gpu.color_offset` at
the moment of the present, which is after the poll returns. Consumption stops
at a flip only after finishing its bounded 0x8000-byte step, so between the two
the parser walks on -- into the next frame's clear, and past whatever rebinds
`color_offset`. A blank file there says nothing about what was displayed.

**Two: three instruments shared one file series.** The report, per-batch and
flip captures all wrote `<prefix>NNN.bmp` from a single counter. Of the 46 files
in claude-flipcorr-52, 24 were flips and 22 were report snapshots taken
mid-composition, which is the one sampling moment already known to be
meaningless for presentation. Files now carry their instrument's name:
`reportNNN`, `drawNNN`, `flipNNN` (live) and `snapNNN` (presented).

Measured over 110 s, claude-fliporder-53, capturing both buffers at each flip:

    live surface at present time (flipNNN) : blank 10 / 24
    presented copy   (snapNNN)             : blank  1 / 23

The one blank present is the frame the trace shows drew a single triangle,
during the change of scene. Everything else carries a picture, and consecutive
captures differ: snap019-023 are the JSRF title screen, correct, centred and
legible, with the logo animating between them, and snap014 is the SEGA
anti-graffiti screen pixel-correct. **The acceptance for goal 3 is met.**

### What the guest emits between its last draw and its flip

`RECOMP_FLIP_TRACE=<stride>` traces inside the FLIP_STALL dispatch: the bound
surface, triangles and clears since the previous flip, the presented copy's
non-black count, and the last 48 dispatched methods. Thirty flips over 110 s,
zero faults. The tail of the command stream is the same every time:

    ... SET_LIGHTING_ENABLE SET_SPECULAR_ENABLE SET_LIGHT_CONTROL 0x17C4
        FLIP_INCREMENT_WRITE NO_OPERATION FLIP_STALL

No CLEAR_SURFACE and no SET_BEGIN_END anywhere near the boundary, so the
clear for frame N+1 does **not** precede the FLIP_STALL for frame N. Stopping
at the flip is the right boundary, and the open question from the last handover
is answered in the negative.

Clears and triangles between flips are 1-3 and 5 respectively on the title
screen -- the "about 750 triangles per capture interval" figure was over a
stride of 150 frames -- and 1 clear, ~2350 triangles per frame in the scene
that follows.

### The surface at the flip is 0x0071E000, and that revises fix 6

At all thirty traced flips the bound surface is 0x0071E000. That is the address
PCRTC_START held, and the address `RECOMP_GUARD_PAGE` caught `xbox_HeapAlloc`
zeroing -- which is what allocating a flip chain out of the heap looks like, not
proof of a stray allocation. So "the framebuffer probe was summing a heap
allocation" (f0dc0dd) was too strong: the address was the presented buffer, and
what the probe lacked was a moment, not an address. Pointing it at
`nv2a_pb_exec_surface_va()` swapped one wrong sample for another -- 115 of 115
samples in claude-fliporder-53 read 0x005F0000, a buffer being composed into,
never the one on screen. The probe now reports the presented copy's non-black
count alongside, which is the number that means "there is a picture".

### The whole run, as the presented copy sees it

`presented nonzero` once a second over 110 s, claude-fbpresented-54, zero
faults and zero bad headers:

    -1 31159 31159 307200 307200 18098 18098 18098 307200 307200
    42349 x22                      the SEGA anti-graffiti screen, held
    307200 1 1 2 1                 white flash, then the change of scene
    9651 74417 112154 44365 ...    the title logo animating in
    44244 +/- a few, x30           the title screen, with something moving
    64356 71025 79313 84604 109464 36249 8141 1 2 2
    0 x11                          the last eleven seconds are black

The port draws its own screens, presents them, and moves between them on its
own. What the last eleven seconds are -- an attract movie we do not draw, a
fade, or a new stall -- is the next question, and it is a different one.

## 4. Verify controls and reach gameplay

Connect the existing host input path, confirm guest-visible button/axis changes,
use controls to enter gameplay, and verify controllable movement in a rendered
scene. Record the tested route and remaining visual/audio/logic defects.

### The input path is already built and measured; what is untested is reaction

Not a green field. CLAUDE_HANDOVER_2026-09-04_USB.txt records the chain working
end to end against real hardware (claude-usb-realpad-01): SDL2 opens a pad, and
presses arrive correctly decoded through the emulated Xbox gamepad, OHCI and
XPP, with zero unhandled control requests and zero faults over 40 s. The
emulated controller enumerates and the title polls it steadily, both by
GET_REPORT and on the interrupt-IN endpoint.

What was never confirmed is that the title *reacts*: with and without a pad the
stage sequence and pushbuffer profile were identical. That was left to the
renderer because there was no picture to read. There is one now, so the next
step is to press buttons at a screen we can see and watch for a change --
`jsrf_title_state_probe` is the trace that says whether the state machine moves.

Two host notes: `open_controllers()` runs once in `xbox_InputInit` with no
hotplug, so attach the pad before launching; and this diagnostic harness has no
window focus of its own to steal.

### Measured 5 September: input reaches the title, and audio is what stops it

Two blockers were in the way and both are fixed. `RECOMP_OHCI_ATTACH=1` puts
the emulated pad on the port at all. And the address-contract change had broken
enumeration outright: the driver's descriptors now carry contiguous-window
addresses (`HcControlHeadED 0x009E29A0 -> 0x808E29A0`) that the service rejected
against its flat RAM window, so it walked zero endpoints (3bf1f29).

With those, a real DualShock 4 over USB reaches the title: 78 button edges
delivered through host backend -> USB device -> OHCI -> XPP in one session, and
366 in a `RECOMP_FAKE_PAD=1` run. The user reports presses working at the
anti-graffiti screen.

**Then it hangs, every time, at the title screen.** Sampled live
(claude-realpad-64): the main thread is 100% inside `sub_001A308E`, which is

    loc_001A3094:  test word [ecx+0x12], 0x8000
                   jne  loc_001A3094

a spin waiting for bit 15 to clear. It is in DSOUND (0x0019E340..0x001BAB1C).
The bit is cleared by the neighbouring servicing routine at loc_001A2FBE, and a
DSound worker thread exists and is alive but spends 1909 of 1939 samples asleep
in `bridge_KeWaitForSingleObject`.

Because the spin never blocks, the main thread never reaches the kernel
bridge's wait, so nothing pumps and nothing submits: the pusher freezes with
`put == get` and the window keeps showing the last composed frame. Every
"it froze" and every beachball in this session is that spin.

**There is no host audio sink on this platform at all:**

    [APU] XAudio2 unavailable, falling back to waveOut
    [APU] waveOutOpen failed (error 11)
    [APU] DSP GP/EP initialized (STUBBED - passthrough mode)

XAudio2 and waveOut are both Windows APIs; nothing in the tree opens CoreAudio
or SDL audio. So the title has been running with its audio decoded into
nothing, which is also why there has never been any sound. SDL3 is already
linked, so a sink is available to write.

Hypothesis, not yet measured: the voice-state bit is cleared by a servicing
path that cannot complete without a working audio pipeline, and the fix is a
real sink rather than anything in DSOUND. Prove it before writing code -- find
what clears bit 15 on hardware and what the worker thread is waiting for.

### The missing backgrounds are dropped draws, not a missing copy

The user's reference shot of the real title screen is the JSRF logo composited
over a live 3D city; ours draws the logo on black, and the earlier logo screens
are missing or cropped backgrounds the same way.

It is not a surface or copy problem. `RECOMP_FLIP_TRACE` now reports the
non-black count of *every* surface the guest has ever bound, at each flip, and
they agree with each other throughout -- at the title screen all three sit at
45k-69k of 307200. Nothing anywhere holds the city.

It is not the transform either. Over the title screen the executor rasterises
175,000-390,000 triangles per five seconds while skipping only 75-170 batches
as not screen-space.

The draws are **dropped at the texture stage**. `prepare_texture_copy` returns
an error and `draw_primitive` does `return` -- the batch is never rasterised at
all -- and the rejection tally over a 130-second run is:

    466,676  multiple textures
    120,879  combiner / texture program
     77,740  blending
     17,914  texture format / mip layout
     10,362  stencil / fog / polygon / logic op

So the executor draws what it can texture with a single stage and a simple
combiner -- logos, text, UI -- and silently drops everything multi-textured,
which is the whole city. That is exactly "foreground renders, background is
black or cropped".

**And it explains the black screen after ~100 s too**, which is a separate
mechanism worth not confusing with the first: there the skipped-batch counter
rises to meet the triangle count exactly (103 tris / 103 skipped, 83/83,
120/120), so that scene is fixed-function and untransformed, and the executor
has no fixed-function T&L. Two different holes, both in the CPU rasteriser.

Neither is a defect to fix in the executor by guesswork. xemu implements the
register combiners, multitexturing and fixed-function T&L completely, and is
the reference to read for both.

### Frame rate, measured

    anti-graffiti screen   45 fps
    title screen           12-16 fps   (~2350 triangles/frame, CPU rasteriser)
    after the spin starts   0 fps

Slow, and expected: the executor rasterises on one core. Not a defect to chase
before the hang.

## Constraints and definition of done

Work in the current macOS harness; no upstream merge or full regeneration is
needed by default. Preserve existing uncommitted work and permanent fault
visibility. Run relevant regression checks for each behavioral change. The
main goal completes only after sustained displayed frames and interactive
gameplay are demonstrated, not merely after startup or another wait is fixed.
