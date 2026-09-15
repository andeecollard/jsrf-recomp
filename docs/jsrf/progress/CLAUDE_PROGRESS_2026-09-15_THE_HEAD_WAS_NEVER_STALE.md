# The head was never stale, and the self-link guard was never measured

15 Sep 2026, evening. Continues
`CLAUDE_PROGRESS_2026-09-15_VOICE_LIST_SELF_LINK.md` and section 3 of
`CLAUDE_HANDOVER_2026-09-15_THE_SOUND_ENGINE_WAS_SWITCHED_OFF.txt`, and
**retracts the chain both of them end on**. No code changed for this note: every
number below is a re-reading of logs already in `build-macos/.../play/` and
`build-macos/.../measure/`, which is the point — the runs that settle this were
taken hours before anyone drew the wrong conclusion from them.

## The claim being retracted

The handover states the producer as a five-step chain:

> the guest writes `link(v) = v` as its "not in any list" sentinel before
> re-ONing `v`; our `regs[top]` still names `v`; `VOICE_ON` stores
> `link(v) = regs[top] = v`; the walk meets a one-entry cycle on a dead voice
> and traps.
>
> **WHY `regs[top]` IS STALE IS NOT ESTABLISHED. That is the open question.**

Step one is wrong, and with it the framing of the open question.

## What the logs say

`[VOICE-LINK]` prints `link=BEFORE->AFTER` for every `VOICE_ON`, where `BEFORE`
is `link(v)` as the insert found it. If the chain above were right, a
cycle-forming ON would show `BEFORE == v` — the sentinel, already written by the
guest.

Classify **only the ON that first self-links each voice**, across all four
instrumented runs. The later ones are the absorbing state re-counted and say
nothing about how it formed:

| run | voices that formed a cycle | `BEFORE == v` (sentinel) | still linked |
|---|---|---|---|
| `20260915-122828-human-gameplay` | 5 | **0** | 5 |
| `20260915-120746-topwrite` | 3 | **0** | 3 |
| `20260915-121615-relink` | 4 | **0** | 4 |
| `20260915-120238-feav2` | 4 | **0** | 4 |

**Not one of the sixteen.** Every voice that formed a cycle was still linked
into the list at the moment of the ON — twelve pointing at another voice, four
at the `FFFF` terminator, which is the last element of a list and not absence
from one:

    human-gameplay   v2->v1  v3->v2  v4->FFFF  v7->v3  v13->v4
    topwrite         v5->v2  v6->FFFF  v71->v67
    relink           v2->v4  v5->v9  v6->FFFF  v71->v67
    feav2            v2->v4  v5->v8  v6->FFFF  v71->v67

The sentinel reading comes from counting *all* self-linking ONs, where it is
343 of 358 in the gameplay run — because once `link(v) == v` every later ON
reads `v->v` and is scored as a sentinel. Counting the absorbing state is what
made a consequence look like a cause.

## What actually happens, from one run, end to end

`20260915-122828-human-gameplay`, the 3D list, in log order:

    9370  [VOICE-TOP]   list=3D 0003 -> FFFF          guest empties the list
    9393  [VOICE-LINK]  voice=3 link=0003->FFFF       ON 3   head FFFF -> 3
    9411  [VOICE-LINK]  voice=1 link=0001->0003       ON 1   head 3 -> 1
    9417  [VOICE-LINK]  voice=4 link=0004->0001       ON 4   head 1 -> 4
    9444  [VOICE-RELINK] voice=4 ours=0001 now=FFFF at=walk
    9510  [VOICE-LINK]  voice=4 link=FFFF->0004 SELF-LINK

At 9444 the guest rewrites `link(4)` from 1 to `FFFF`. That is two
`RemoveIdleVoice` calls — voice 1 removed (`link(4) = link(1) = 3`), then voice
3 (`link(4) = link(3) = FFFF`) — with the shadow reporting only the first
divergence per voice. The guest is maintaining the list **correctly and
actively**, and it leaves voice 4 in place, because voice 4 is the head and has
not idled.

Both views now agree exactly: head = 4, `link(4) = FFFF`, a one-element list.

Then at 9510 the guest ONs voice 4 again. `regs[top]` is 4 because voice 4 **is**
the head — we put it there at 9417, nothing has removed it, and the guest's own
relink at 9444 treats it as the live head. `link(4) = regs[top] = 4`.

`regs[top]` is not stale. There is no missing head write, no failed handshake,
and nothing for the guest to have done differently. **The producer is a
`VOICE_ON` for a voice that is already its list's head**, which is an ordinary
thing for DirectSound to do: an app replaying a buffer whose voice has stopped
but has not yet been taken out of the hardware list.

## Which makes the self-link a one-element list, not corruption

A TOP insert of the current head yields `TVL = v`, `link(v) = v` on hardware
too — the insert is byte-for-byte xemu's, and that was checked against the
driver last session and is not in question. So either a real Xbox forms the same
cycle and survives it, or it reads `link(v) == v` as the end of the list.

The driver's own convention answers it: `SetupVoiceProcessor` writes
`link(v) = v` for all 256 voices at boot and `RemoveIdleVoice` writes it again
for every voice it takes out, both meaning "not in any list". A list terminated
that way is the only reading under which the title plays on hardware at all.

That is exactly what `RECOMP_APU_SELFLINK_END` implements. At the ON the voice
is made **active**, so a correctly terminated `[v]` renders it and stops, and no
trap is owed at that point.

**And there is a real argument against it, which this finding creates rather
than removes.** The case made for the guard last session was that terminating
cannot lose a reachable voice, "because by the time the guest re-ONs `v` it has
already written `link(v) = v` itself, so whatever `v` used to point at is
unreachable through `v` regardless". That premise is the one the table above
kills. At formation `link(v)` held a real successor — voice 67, voice 3, voice
1, or the `FFFF` terminator — and **our own insert is what overwrote it**.
Terminating at `v` therefore discards whatever the guest still had behind it.

Not terminating discards the same tail *and* pins the walk, so the guard is not
worse. But neither is lossless, and both losses are caused by the insert
overwriting a live link. That points at the insert after all — at a `VOICE_ON`
for a voice already at `TVL` — and the reason it was ruled out last session
(ours is byte-for-byte xemu's) answers a different question: whether we diverge
from xemu, not whether the state reaching the insert is one hardware ever sees.
The unexplained part is now sharp and small: **our `regs[top]` names `v` at the
re-ON and the guest's own list evidently does not, or it would have written
`TVL` when it took `v` out.** Where those two views part company is the next
thing to measure.

## And that switch has never been measured

`RECOMP_APU_SELFLINK_END` is default OFF, on this finding in the handover:

> It fired 47,293 times in 240 s and left trapped at 50.3%, indistinguishable.

Its A/B is `measure/selflink_ab_20260915-125422`, four runs:

    t1_guard0  on=5    idle_trap=0      trapped   0/45032  ( 0.0%)  fired  12/199
    t1_guard1  on=233  idle_trap=18005  trapped 203439/404456 (50.3%)  fired 199/199
    t2_guard0  on=5    idle_trap=0      trapped   0/45032  ( 0.0%)  fired  12/199
    t2_guard1  on=5    idle_trap=0      trapped   0/45024  ( 0.0%)  fired  12/199

**One run in four reached voice churn, and it is in the guard=1 arm.** The other
three fired 12 of 199 pad events — the guest stopped reading the pad at about
t=15 s, which is the input-poll stall this tree already documents, and `on=5
off=0 idle_trap=0` is the parked-player signature `gameplay_nobarrage.pad`'s own
header names. There is **no guard=0 run with churn anywhere in the A/B**, so
50.3% has nothing to be indistinguishable from. The schedule was `gameplay.pad`
(199 events), the one that lands in the VS Battle menu.

### And the control arm could not have worked anyway

The statistical problem above is the smaller one. `mcpx_apu_selflink_end()` read

    on = getenv("RECOMP_APU_SELFLINK_END") != NULL;

so `RECOMP_APU_SELFLINK_END=0` — which its own comment eight lines earlier
offers as the way to "restore the previous behaviour for A/B" — **enabled the
guard**. Both arms of that A/B ran with it on. Both arms said so, in every
report:

    [APU-SELFLINK] terminated=214679 (guard on)      <- the RECOMP_..._END=0 arm
    [APU-SELFLINK] terminated=199861 (guard on)      <- the RECOMP_..._END=1 arm

Nobody read the line. Reproduced on 15 Sep in a fresh pair of runs before the
fix, which is where those two figures come from, and it is now value-tested like
`mcpx_apu_trap_coalesce` and `mcpx_apu_se_while_trapped` beside it. The other
two switches in this tree documented as `=0`-disableable — `RECOMP_GPU_OWN` and
`RECOMP_PHYSICAL_HEAP_ALIAS` — were checked and honour it. The presence test is
right for a trace and wrong for an A/B switch, and this tree has ~30 of the
former.

`ab_score.py` now harvests the state each report names and refuses to compare
two arms that report the same one. Its positive control is the pair above.

The retraction in the handover — "the self-link guard is the fix" corrected to
"it fires 47,293 times and moves nothing" — replaced a wrong conclusion with an
unmeasured one. The honest statement is that the switch is **untested**.

## Two divergences in the walk, and the second is new

Both are in `mcpx_apu_vp_frame`, and both appeared with the local cursor that
`RECOMP_APU_SE_WHILE_TRAPPED` needed. On hardware the guest learns which voice
idled from `CVL`, and `RemoveIdleVoice` repairs `CVL` **and** `NVL` to steer an
in-progress walk; the pair has to hold still from the trap until the guest
clears it.

**1. `NVL` runs on while `CVL` is frozen.** `d->regs[next] = nxt;` executes on
every iteration, including every iteration after `trap_held` is set. So `CVL`
holds the idle voice while `NVL` advances to the end of the list. The pair the
guest reads is incoherent — `CVL = v`, `NVL` = the tail, rather than
`NVL = link(v)`.

**2. `CVL` is overwritten on the next frame, while the trap is still
outstanding.** With `SE_WHILE_TRAPPED` on — the default since this morning —
`apu_active` is true while `FEMETHMODE == TRAPPED`, so the frame thread calls
`mcpx_apu_vp_frame` again, and it opens every list with an unconditional

    d->regs[current] = d->regs[top];

Before that switch the frame thread idled on `TRAPPED` and the pair froze by
construction, which is xemu's behaviour and hardware's. The change that took the
sound engine from 63.6% to 98.6% duty took the register pair with it.

Neither is the producer of the storm — the storm predates both, and
`20260915-110630` shows it with the engine idling on `TRAPPED` — but item 3 of
the handover asks for exactly this and it is worth fixing on its own terms: the
guest cannot steer a walk through registers that move underneath it.

## What I would do next, in this order

1. **A/B `RECOMP_APU_SELFLINK_END` properly**, with `gameplay_nobarrage.pad`,
   the scene gate and a control arm that actually churns. `ab_switch.sh` exists
   for this now and refuses to compare a parked run with a churning one.
2. **Freeze the `CVL`/`NVL` pair while `FEMETHMODE == TRAPPED`** — stop writing
   `NVL` once `trap_held`, and do not reload `CVL` from `TVL` while a trap is
   outstanding. The rendering cursor stays local, so the duty-cycle win is
   untouched.
3. Only then ask whether the walk should **resume** from `CVL` rather than
   restart from `TVL` after a trap clears. That is the rest of item 3 and it is
   a behavioural change, so it wants its own switch and its own A/B.

## Method notes

Two things in this note were only findable by not trusting a summary.

**Count the transition, not the state.** 343 of 358 self-linking ONs carry the
sentinel and zero of the sixteen cycle-forming ones do. Any counter that
aggregates over an absorbing state reports the state, and the state is the
consequence.

**An A/B with one usable arm is not an A/B.** Both `RESULTS.txt` lines and the
handover read as a four-run comparison. `on=5` in three of them is the tell, and
it is the same signature the pad files have been warning about for two days.
