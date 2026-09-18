# Four voices, still on, and silent — 18 September 2026, night (5)

Analysis of the 22:30 player session against the 22:25 one. No runs were made;
this is entirely re-reading two preserved logs.

## The music is four voices that never stop and never retire

`[VOICE-LIFECYCLE]` gives one event per voice per transition. Voices **64, 65,
66 and 67** have **exactly one event each — `event=on` at `audio_frames=256`
(t ≈ 0.005 s) — and no idle, no off, no retire, in either session.** Four
voices, matching the four guest writes to the 2D list head.

They are started before the title has drawn a frame and are never touched
again. In the healthy session they produce for the whole run. In the broken
session they stop producing at window 7 and never resume, **while still on**.

## The shape of the failure is a failed RESTART, not a death

Per-window deltas, broken session:

    win 3-7    2D +9040 .. +18358      3D sporadic
    win 8-15   2D +0                   3D +0        <- a transition silences BOTH
    win 16-43  2D +0  (36 windows)     3D +4494 .. +20514 sustained

Both bins stop together at the transition the player described as "at Gum".
**3D comes back at window 16. 2D never does.** An earlier reading of this as
"2D froze while 3D climbed" was taken off a live tail and missed that 3D
stopped too.

## Four explanations tried and rejected tonight, three of them mine

| idea | how it died |
|---|---|
| The guest stops issuing APU methods (G1's title) | `guest_methods` runs at +2000–3300 per window across the whole silent period. |
| The guest stops **starting** 2D voices | `on_2d=10` and flat — **in the healthy session too**, where the music plays fine. Same starts, different output. The healthy control is what caught this. |
| The music voices were retired or turned off | v64–v67 have no retire, no off, no idle event in either session. |
| ADPCM decode failure silencing the music (G6's mechanism) | Failures occur in both sessions. The **largest burst (+5999) coincides with the healthiest music window (+18358)**, and failures fall to zero after window 10 because nothing is being decoded any more. |

## What is left, and the instrument it needs

Four voices are ACTIVE by the model's own bookkeeping, are in the 2D list, and
produce nothing. The remaining shapes are:

- their `PAR_STATE` lost `ACTIVE_VOICE` without an event being emitted;
- they are active but their buffer/cursor stopped advancing;
- they are active and advancing over data that is silent or unreachable.

**No existing instrument can separate those**, and the reason is structural:
`[VOICE-LIFECYCLE]` is *event-driven*, and these four voices emit no events
after t=0.005 s. The one instrument pointed at 2D voices, `[APU-POOL] on_2d`,
counts starts — which G5 has said since 17 Sep is useless for exactly this,
and which is now load-bearing rather than cosmetic.

**What is needed is small and specific:** a periodic dump, once per report, of
every voice in the 2D list — handle, `PAR_STATE`, the active bit, the buffer
cursor and the link word. Four voices, once every five seconds. That would
have answered this session in one line.

## The link to the hang, and what is missing from it

The same session hung with the process alive, and two samples 30 s apart put a
guest thread in the self-suspend path of `SuspendThread` waiting for a resume
that never came, with `sub_00147DAC` below it — the frame `win32_compat.c`
names in its own comment as JSRF's XAPI worker.

A coherent chain exists: the transition suspends an audio worker, the resume
is lost, the four music voices stop being fed, and they play out and go silent
while remaining on. **Every link has evidence except the one that matters** —
that the parked thread is the one feeding those voices. Nothing attributes
voice work to a thread, and `g_w32_parked` / `g_w32_suspends` are incremented
and never printed.

Do not promote this to a finding until that attribution exists.
