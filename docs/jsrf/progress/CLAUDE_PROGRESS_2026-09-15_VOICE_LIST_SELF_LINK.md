# The trap storm is a one-entry cycle in the hardware voice list

15 Sep 2026. Binary `-O2`, gen `f8afd85588c196d0` (current), built in
`build-macos/jsrf-first-fault/build-feav`. `ctest` 28/28 on that build at each
step of the instrumentation, and 28/28 on the uninstrumented `build-new`.

Continues `2026-09-15-audio-lifecycle.md`, which narrowed this to "list
ownership/reuse and unlink ordering" and named the next test: whether `ON`
inserts a voice that is already linked. It does. This note answers that test and
records four hypotheses it killed, three of them mine.

## What was measured

Three runs, `pad/gameplay.pad` driving them unattended — the schedule that
skates and sprays rather than parking the player, so voices actually retire.

**They did not reach gameplay, and that bounds every claim below.** A person
looked at the window during the fourth run and found the title sitting in the
VS **Battle mode stage-select menu**. The boot prefix is timing-fragile by its
own admission — `new_game.pad`'s header says the logos vary by ten seconds or
more between runs — and the `A` that should take New Game took multiplayer
instead. All three runs show `NtOpenFile` = **131**, against 1342 for the title
plateau and 1408 for New Game. `ord175` was 63,794–67,926, so input was healthy
throughout; the title was not stalled, it was in the wrong branch of the menu.

What the runs therefore measure is the voice list under the Battle-mode menu's
own churn — cursor and stage-preview sounds — which turns out to exercise it
hard: 196–233 ONs and 179–226 retirements, more than any scripted run in this
repository has previously produced. The mechanism below is established on that
traffic, and it is the same code either way.

What is **not** established is that this mechanism produces the gameplay trap
storm. That storm is real and separately measured — `20260915-110630`,
`NtOpenFile` 1410, `ord175` 73,645, v3 raising 19,552 of 19,585 traps — but that
run predates this instrumentation and carries none of it. The two have not been
joined, and joining them needs an instrumented run that actually reaches a
stage. Until then this is a mechanism demonstrated in a menu and a storm
observed at gameplay, with a plausible but unproven identity between them.

| run | length | on | off | off_commands | idle_trap | self_link |
|---|---|---|---|---|---|---|
| `20260915-120238-feav2` | 260 s | 233 | 226 | 222 | 18879 | 35 |
| `20260915-120746-topwrite` | 260 s | 206 | 195 | 194 | 15377 | 27 |

Both: `antecedent_sets` equal to `on` at **every** report, `on_top` equal to
`on`, `on_inherit=0`.

`20260915-120746-topwrite` also carries the list-head counters:

    [VOICE-TOP] head writes from guest: 2D=7 3D=25 MP=1
    [APU-WRITE] main=46624 vp=285038 gp=7 ep=8 other=0

**33 head writes against 194 explicit guest retirements.**

The steady-state cost is from the saved run `20260915-110630`, which is where
the reported v3 storm lives. Differencing its last two 30 s reports:

| | total subframes | sound engine ran | trapped |
|---|---|---|---|
| t=270→300 | 45008 | **3709 (8.2%)** | 40876 (90.8%) |

Not the 48% the cumulative figure in `ACCURACY_GAPS.md` reports — that number is
diluted by a healthy first two minutes. By t=210 the engine is pinned at one
subframe in twelve and stays there. In the same run `v3` accounts for **19552 of
19585** idle traps, at a dead-constant 123.6 raises/s, and the last-sixteen ring
is `3 3 3 3 3 3 3 3 3 3 3 3 3 3 3 3`.

## The mechanism

A self-link — `link(v) == v` — is a one-entry cycle in the list
`mcpx_apu_vp_frame` walks. The walk finds the voice inactive, raises
`SE2FE_IDLE_VOICE`, and returns; `d->regs[current]` is deliberately left
pointing at that voice so the guest can read which one idled. Next subframe the
walk restarts there, finds the same inactive voice, and raises again. Everything
behind it in the list is never reached, and so never rendered.

It is produced by `VOICE_ON`'s TOP branch, `apu_vp.c`:

    link(selected) = regs[top];
    regs[top]      = selected;

which self-links exactly when `regs[top]` already names `selected` — that is,
**two `VOICE_ON`s for the same voice with no head write between them.**

That is readable straight out of the trace, because on the TOP branch the
`link=BEFORE->AFTER` the trace prints has `AFTER` equal to whatever the head was
at that moment. From `20260915-120746-topwrite`, the 2D list:

    VOICE-LINK voice=71 link=0047->0044     ON 71, head was 68
    VOICE-TOP  list=2D  0047 -> 0044        guest moves head off 71   OK
    VOICE-LINK voice=71 link=0047->0044     ON 71, head was 68
    VOICE-TOP  list=2D  0047 -> 0044        guest moves head off 71   OK
    VOICE-LINK voice=71 link=0047->0044     ON 71, head was 68
    VOICE-TOP  list=2D  0047 -> 0044        guest moves head off 71   OK
    VOICE-TOP  list=2D  0044 -> 0043        head 68 -> 67
    VOICE-LINK voice=71 link=0047->0043     ON 71, head was 67, head now 71
    VOICE-LINK voice=71 link=0043->0047     SELF-LINK  <- no head write between
    VOICE-LINK voice=71 link=0047->0047     SELF-LINK
    VOICE-LINK voice=71 link=0047->0047     SELF-LINK   (absorbing from here)

Every earlier repeat-ON of voice 71 is preceded by the guest moving the head off
71. The one that self-links is the one where that write is absent. Once formed
the state is absorbing: every later ON reads `0047->0047`.

## The contrast between the lists

The 3D list gets a head write after almost every ON — the guest walks it back
down to the `FFFF` terminator each time:

    VOICE-TOP  list=3D 0000 -> FFFF       init, empty
    VOICE-LINK voice=0 link=0000->FFFF    ON 0,  head was FFFF, head now 0
    VOICE-TOP  list=3D 0000 -> FFFF       guest empties the list      OK
    VOICE-LINK voice=0 link=0000->FFFF    ON 0,  head was FFFF        OK
    VOICE-TOP  list=3D 0000 -> FFFF       guest empties the list      OK
    VOICE-LINK voice=1 link=0001->FFFF    ON 1,  head was FFFF        OK
    VOICE-TOP  list=3D 0001 -> FFFF       guest empties the list      OK
    VOICE-LINK voice=2 link=0002->FFFF    ON 2,  head was FFFF        OK
    VOICE-TOP  list=3D 0002 -> FFFF       guest empties the list      OK

25 head writes, and its self-links are occasional. The 2D list got **7** across
the whole run, and that is where voice 71 re-ONs into its own link every ~240k
output frames for the rest of the session.

## Hypotheses that died

This is the part worth reading. Three of the four were mine, and each was
derived by reading code rather than by running it.

**The INHERIT-branch arithmetic.** `VOICE_ON`'s other branch does
`next = link(ante); link(selected) = next; link(ante) = selected`, which with
`ante == selected` collapses to `link(v) = v`. That is real, it is in the code,
and **it never executes**: `on_inherit=0` across 206 and 233 ONs in two runs.
The reasoning was sound and pointed at the wrong branch.

The only reason the real producer was caught is that `self_link` was written as
a **readback of the outcome** — read `link(selected)` after the insert and
compare it to the handle — rather than as the `ante == selected` condition the
hypothesis predicted. A counter that encoded the hypothesis would have reported
zero and confirmed nothing. `self_ante` is kept beside it precisely so the two
disagreeing is visible; they do, and that disagreement is the finding.

**Stale FEAV — a dropped `SET_ANTECEDENT_VOICE` through the trapped-page
window.** Dead. `antecedent_sets` equalled `on` at every report in both runs —
`feav2` 5, 12, 16, 41, 74, 116, 157, 196, 233 and `topwrite` 5, 12, 16, 41, 81,
122, 167, 206, each matching `on` exactly. If stores to that page were being
lost this counter would trail. It never does.
This was an attractive hypothesis because it is the bug class that already ate
`HcInterruptEnable`, and being the right shape is not evidence.

**"The guest never writes the list head."** Wrong, and worth recording as a
method failure rather than just a wrong answer. It was asserted off a sample
taken at t=30 s, before any voice churn, when the only head writes in the log
were the three `0000 -> FFFF` initialisations. The guest writes the head 33
times once the title is actually retiring voices. Sampling an absence during the
quiet part of a run and calling it an absence is the same error as trusting a
counter without reading its trigger.

**The freed-object framing for the intermittent crash.** Still unobserved, and
now weaker: both runs produced the full storm with **no guest crash**. The
storm does not require a freed object, so the storm is not evidence for one.
The other session's note said the same thing from the other direction and was
right to.

## What is not established

**Why the head write is absent at that one point.** Guest logic that skips it,
or a store that was issued and lost — these runs cannot separate them. The
positive control says the aperture is not deaf in general (`main=46624`
register writes arrived, 33 of them head writes with sensible values), but that
does not exonerate the specific store.

**A second corruption shape the counter cannot see.** In
`20260915-120746-topwrite`, two consecutive ONs leave `link(68)=69` and
`link(69)=68`:

    VOICE-LINK voice=69 link=0045->0044     link(69) = 68
    VOICE-LINK voice=68 link=0044->0045     link(68) = 69

That is a two-element cycle, and `self_link` does not catch it because neither
handle points at itself. The walk's only protection against any cycle is the
`i >= MCPX_HW_MAX_VOICES` iteration cap — for an inactive voice the trap
returns before that is reached, but a cycle of *active* voices would render each
of them up to 256 times per subframe before bailing out. Whether this pair
persisted is not established: see below.

## What the instrument had to be, and why

`voice_set_mask` writes through `stl_le_phys`. **The voice register file lives in
guest RAM**, not in `d->regs`. So the guest edits a `PITCH_LINK` with an ordinary
store: no MMIO, no method, nothing an MMIO write hook can observe, and — worth
stating plainly — nothing a trapped-page window could swallow. Only the three
list heads (`NV_PAPU_TVL2D/3D/MP`) are registers, which is why the head writes
are countable and the link writes are not.

A write trace is therefore impossible and a **differential** one is easy: shadow
the value this model last wrote for each voice, and report it whenever a
readback no longer matches. A mismatch is someone else's store, and the only
other writer is the guest. That is `[VOICE-RELINK]`, hooked at `VOICE_ON` and at
every link read in the walk, reporting the first divergence per voice so the
walk's per-subframe reads cannot bury the transition.

One trap the other session's note flagged in advance and saved us from: **a
voice's `PITCH_LINK` defaults to its own handle.** Every voice in the 2D trace
shows `link=0040->`, `0041->`, `0042->` on its first ON — the before column
holding its own handle. A self value in a pre-operation trace is the initial
state, not corruption. `self_link` keys on the *after* value and is unaffected,
but the before column cannot be read naively, and reading it naively is exactly
the error that note predicted.

## The front end was dropping methods silently

Separate from the above, found while tracing it. `fe_method`'s unknown-method
branch was:

    } else {
        /* Unknown method - silently ignore */
        DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
    }

`DPRINTF` compiles to nothing. So every front-end method the model does not
decode was dropped leaving no count and no trace, which means "the guest never
asked" and "we ignored the ask" have been **indistinguishable in every log in
this repository**. That is the wrong shape for a front end whose entire job is
to receive guest commands. Now counted, with the distinct method numbers kept
and named in the report. The method is still ignored; only the silence changed.

## Did the guest edit the links behind us?

**Yes, and it owns them.** Run `20260915-121615-relink`, 260 s, same build and
gen: `on=196 off=180 off_commands=179 idle_trap=15412 self_link=29`, head
writes `2D=6 3D=29 MP=1`, and **69 links changed behind the model over
1,735,680 reads**. The shadow reports the first divergence per voice, so 14
distinct voices were named out of those 69.

Every named divergence falls into one of two shapes, and both are correct
DirectSound behaviour:

| shape | count | seen at |
|---|---|---|
| guest sets `link(v) = v` | 11 | `voice-on` |
| guest relinks a predecessor past a removed voice | 3 | `walk` |

    [VOICE-RELINK] voice=69 ours=0044 now=0045 at=voice-on
    [VOICE-RELINK] voice=68 ours=0043 now=0044 at=voice-on
    [VOICE-RELINK] voice=70 ours=0044 now=0043 at=walk

The first two are the 68/69 pair from run 3, answered: the link did not revert
by itself and it was not our code. **The guest writes a voice's own handle into
its link before re-ONing it** — a "not in any list" sentinel. The third is the
guest unlinking voice 68 by moving voice 70's successor from 68 to 67, done
*while the walk was running*.

So there is no second writer to find and no lost store. The guest maintains the
list itself, in RAM, coherently.

**What this makes the defect.** `FEAV` read `0001FFFF` or `0002FFFF` on every
one of the 196 ONs — `FEAV.VALUE` is always `0xFFFF`. The guest never names an
antecedent; it only ever says "put this voice at the top of list N", which means
it tracks the head in its own software structure and never reads `regs[top]`
back. The guest owns the links *and* the head. Our `VOICE_ON` writes both
underneath it, and when the head it wrote earlier still names the voice being
re-ONed, `link(selected) = regs[top]` stores the voice's own handle and the walk
pins on a one-entry cycle.

Run 3 shows the two views diverging in the middle of the list as well, not only
at the head: at `TOP 2D 0046 -> 0043` the guest gives voice 70's successor as
67, while our `link(70)` was 68 — the guest had already removed 68 and we had
not.

**The open question is no longer "why is the head write missing".** It is
whether our `VOICE_ON` should be writing `regs[top]` at all. That is a question
about what the hardware does, and this model is derived from xemu's, so it is
answered by reading xemu — not by experiment on our own guess.

## Scope of the change

Counters, traces and comments. **No trap policy, list behaviour or audio
behaviour was changed** — no automatic unlink, no trap suppression, no
rejection of an ON for a voice already at the head. The case for making
`VOICE_ON` refuse to prepend a voice that is already the list head is now
plausible, but it is not proven until it is known why the head write is missing,
and patching the model around a guest write we have not explained would hide
the producer. Same rule the other session applied to the freed-object
hypothesis.
