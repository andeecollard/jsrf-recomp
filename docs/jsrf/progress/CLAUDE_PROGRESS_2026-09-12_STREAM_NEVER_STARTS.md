# The fifth voice is the music, and on Windows the music is never asked for

Date: 2026-09-12 (Europe/London)

> **RETIRED THE SAME EVENING.** Windows reaches gameplay, opens the BGM and
> reports `on=5` — it needs 672 s, not 110. The runs behind this note were
> too short and the hosts were compared at equal wall-clock time instead of
> equal guest main-loop count. See
> `CLAUDE_HANDOVER_2026-09-12_WINDOWS_REACHES_GAMEPLAY.txt`.
>
> **CORRECTED LATER THE SAME DAY.** The measurements below are sound; the
> conclusion drawn from them was pitched one level too low. The Windows oracle
> **never reaches gameplay** -- 0 of the 17 CPlayer gameplay functions ever
> run, and the title state machine sub_0004EF90 never executes -- so the
> missing BGM open is a symptom, not a root cause, and the whole `on=5` vs
> `on=4` comparison was a gameplay scene measured against a pre-gameplay one.
> See `CLAUDE_HANDOVER_2026-09-12_WINDOWS_NEVER_REACHES_GAMEPLAY.txt`.
> Section "The confound, and why it does not explain this", below, is the part
> that was wrong: its evidence is real and does not support its conclusion.

Long form behind `CLAUDE_HANDOVER_2026-09-12_ADX_STREAM_NEVER_STARTS.txt`.
Supersedes the audio half of
`CLAUDE_PROGRESS_2026-09-11_CONTIG_ALIASES_THE_HEAP.md` and of the
`WINDOWS_RENDERS_FOR_REAL` handover.

Everything here is a paired run: one gen tree, one instrumented build,
cross-compiled for both hosts from the same sources, same scene, same session.


## What was being chased

`on=5` on macOS against `on=4` on Windows. The previous handover had followed
it to two functions that configure voice handle 0x44 and never run on Windows,
and to two feeding methods 32 bytes apart, with the prediction that "a single
gate above both is the shape to look for".

The prediction was right. The gate is four levels higher than where the search
had reached, and it is not in DirectSound at all.


## The chain, measured end to end

    D:\Media\Z_ADX\BGM\ opened
      -> ADX stream slot 0, byte [slot+1]: 0 -> 1 -> 3
        -> sub_0013EF00 reaches sub_0013ECE0            (needs (stat>>1)&1)
          -> call [[slot+0x38]+0x20]  ==  sub_001417B0
            -> sub_0019F1F4                             (GetCurrentPosition)
              -> sub_0019ECA3 -> sub_001A43DA -> sub_001A3570
                -> SET_CURRENT_VOICE 0x44               -> on=5

On Windows the first link never happens and every link below it is
correspondingly dead. On macOS the two `D:\Media\Z_ADX\BGM\` opens land at
lines 6119 and 6123 of the log and the `0 -> 1` transition at line 6130, eleven
lines later.


## The four levels, and what each run said

### 1. The DSOUND fatal latch: exonerated on both hosts

`0x001BA04C` is read at forty-eight sites across DSOUND's API surface and
written at exactly one, `0x001A2317` inside the vtable thunk `sub_001A230D`.
Every reader returns `0x80004005` when it is set. It is never cleared. That is
a textbook single gate above both missing writers, and it is not the bug:

    macOS    [1BA04C]=00000000 for the whole run, 001A230D: 0 calls
    Windows  [1BA04C]=00000000 for the whole run, 001A230D: 0 calls

`blocked=0` at all four reader sites on both hosts. `RECOMP_DSOUND_FATAL=1`.

What the same run did show is the real boundary:

    site        what                       macOS   Windows
    0019F1F4    api thunk -> ... -> 3570     1604         0
    0019ECA3    gate above 001A43DA          1604         0
    0019F214    api thunk -> ... -> 49A7      204        63
    0019EDCE    gate above 001A49A7           204        63
    001A230D    the latch                       0         0

Both hosts make the *same* first 63 calls through the second chain, from the
same caller `0x0016CDB1`. Windows then stops and macOS goes on. The first chain
Windows never enters at all.

### 2. What sub_0019F1F4 actually is

Its macOS caller is `0x001417EA`, which is not in DSOUND. Reading
`sub_001417B0`:

    eax = [handle + 4];  if (!eax) goto fail
    eax = [handle + 8];  if (!eax) { report 0x001DFDD4; return 0 }
    call sub_0019F1F4(eax, &play, &write)
    if (!result) return play / ([handle+0x1A] >> 3) / [handle + 0x24]

Three arguments, a play cursor and a write cursor, divided by a frame size:
`IDirectSoundBuffer::GetCurrentPosition`. The 1604 calls are the ADX streaming
feeder polling its buffer at about 20 Hz. **The fifth voice is the music.**

The string at `0x001DFDD4` is CRI's own: `E1225:dsb(member in handle) is NULL`,
pushed from twelve sites in the `0x00141xxx` driver, all funnelling into
`sub_00143240`, which formats into `0x0026A280` and forwards to a hook at
`0x0026177C` that JSRF never installs. This layer has been describing its own
failures into a dead buffer for the whole project, exactly as `sub_0013C890`
does one layer down.

### 3. CRI's DirectSound driver: never entered, not even to complain

`RECOMP_CRI_DSOUND=1`, whole run:

    site        what                        macOS   Windows
    00143240    diagnostic funnel               0         0
    001417B0    GetCurrentPosition caller     664+        0
    00141560    sibling                         1         0

macOS's handle is the static `0x00272900`, `[+4]=1`, `[+8]=80954B60` -- and
`80954B60` is exactly the `this` seen entering `sub_0019F1F4`. Windows reports
no E1225, because it never gets as far as the check.

Corroboration worth keeping: `sub_00141560` is called exactly once on macOS,
and the previous handover recorded `sub_001A49A7` reached exactly once. The
counts match at 1. Consistent with that path, not proof of it.

### 4. CRI's stream server: identical on both hosts

    sub_0013F080                       the server tick
      if ([0x002615A0] == 1) return;                 re-entrancy guard
      for (slot = 0x0027B1C0, i = 16; i--; slot += 0xA4)
          if (byte [slot] == 1) sub_0013EF00(slot);

    sub_0013EF00                       one stream
      if ([slot + 0x8C] == 1) return;                never taken on either host
      ...
      edx = byte [slot + 1];  edx >>= 1;  edx &= 1
      if (edx) sub_0013ECE0(slot);                   <-- THE GATE

`RECOMP_CRI_SERVER=1`:

    macOS    4 slots in use from tick 1, none gated, ticks=2682 calls=10728
    Windows  4 slots in use from tick 3, none gated, ticks=3809 calls=15236

Windows runs the per-stream body *more* often than macOS. The slots are
created, in use, and ungated on both. Only `byte [slot+1]` differs:

    macOS    slot 0: stat 0 -> 1 (tick 906) -> 3 (tick 931), 3 polls the cursor
    Windows  all four slots: stat 0, for the whole run

`sub_0013ECE0` is called 1533 times on macOS and **zero** times on Windows, and
its vtable dispatch `[[slot+0x38]+0x20]` resolves on macOS to
`vtable=0x0022DC80 -> 0x001417B0`, which closes the chain.


## The confound, and why it does not explain this

"Windows simply has not got there yet" would explain everything above. It does
not hold:

  * Both hosts open the same `Z:\Media\*` directories in near-identical
    proportions (Event 79/76, TalkEvent 79/76, Stage 66/66, Mission 52/52,
    Player 62/51, SE 43/38, Effect 32/32, Enemy 24/24, StgObj 20/20).
  * Both create the same four stream slots within the first three server ticks.
  * Both start four SE voices, and the Windows run is healthy at every other
    instrument: 2577 textures prepared / 0 rejected, VBLANK 62.3 Hz,
    XAudio2 delivering 13,104 buffers.
  * The Windows run was 120 s. macOS starts the stream at about 30 s.
  * `D:\Media\Z_ADX\BGM\` is the ONLY `D:\` subpath either host opens, and
    macOS opens it twice while Windows never does.


## Where it goes next

Not into DSOUND, and not into the APU. The question is now:

  **what opens `D:\Media\Z_ADX\BGM\`, and why does that code not run on
  Windows?**

`0x0027B1C0` is referenced from six places; `sub_0013E550` (called by
`sub_0013F290`) is the slot lookup, `sub_0013E440` and `sub_0013F470` have no
static callers. The status byte `[slot+1]` is written from a dozen sites in
`0x0013Bxxx..0x0013Cxxx` -- but note those may be a different object's `+1`,
so confirm the object before trusting any of them. The transition to watch is
`0 -> 1`, which on macOS happens within eleven log lines of the directory open.


## Retired

  * The `byte [this+0x12] & 3` gate at `0x001A43F0`. Reached only on macOS,
    where it passes. Never the question.
  * `0x001BA04C`. Tested on both hosts with a positive control. Zero
    throughout, latch never fires.
  * "Two adjacent methods on one object." The adjacency at `0x0019F1F4` and
    `0x0019F214` is real, but they are DirectSound API thunks with the MSVC
    `neg/sbb/and` null-check idiom, called from seven and nine game sites
    respectively -- not two methods on one object. The single gate exists, four
    levels higher.
