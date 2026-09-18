# The guest is told about every dead voice, and does nothing — 18 Sep 2026

From the player's 09:00 session with `RECOMP_APU_CYCLE_BREAK=1`, run to the
point where they reported *"Gum says Hey you then the audio stops"*, then left
idling and quit at 09:2x. Archived complete as
`last-run-2026-09-18_0900-cyclebreak-COMPLETE.log` (21.7 MB) before the next
launch could overwrite it.

**The defect reproduced.** `[APU-BIN] 2D heard` is frozen at 97,630 across the
final eight reports and `guest_methods` is frozen at 9,406 — the G1 signature,
in a session the player confirmed by ear.

## G1's measurement, taken at last, and it refutes the hypothesis it was built for

    [APU-IDLE-DELIVERY] found idle=10 distinct, handle delivered=10 distinct,
                        never told about=0: none

The goals have carried this since 17 Sep as *the next measurement*:

> The trap carries ONE handle in FEDECPARAM. […] when several voices retire
> inside one burst, ask how many distinct handles were ever actually DELIVERED
> to the guest against how many went idle. If the answer is "one per burst",
> the others were never named and the guest cannot remove what it was never
> told about.

**The answer is not "one per burst". It is all ten of ten.** Every voice that
went idle was named to the guest in FEDECPARAM. `never told about: none`.

So the guest is not being starved of handles. It is told, correctly, about
every dead voice — and removes none of them. Against that:

    [APU-IDLE-TRAP] raises=103663 ... repeat=103644

103,663 raises, of which **103,644 are repeats** of a handle already delivered.
We name ten voices, the guest acknowledges and declines, and we then ask
103,644 more times.

**This closes the delivery question and reopens the harder one.** The defect is
not in what our model tells the guest. It is in why the guest, having been
told, does not act — which is XDK DirectSound library code inside the DSOUND
section, not the title's own. The levers remain what the APU model and kernel
present to it, but "the handle never arrived" is no longer one of them.

## The cycle hypothesis survives as real, but not as necessary

    [APU-CYCLE] relink=5 (of 23 top inserts) walks_with_a_cycle=0 broken=0
                last=v0/list0 (cycle_break on)
    [APU-WALKCAP] hit=0

The switch was **on** and `broken=0`, so it never fired and changed nothing.
And `walks_with_a_cycle=0` — no rings formed this session.

**Yet the music died anyway.** So a list cycle is *not necessary* for the music
death. That is a stronger refutation than the one in the REFUTED table, which
came from a run where the music never died and nothing was exercised; this one
comes from a session that reproduced the defect.

The 22:40 session's 88 cycles remain real and unexplained. What has changed is
that they cannot be the mechanism on their own.

**Do not read `cycle_break` as tested.** It never fired, so this session says
nothing about whether breaking a ring helps when there is one to break.

## Why this session is weaker than it looks, stated plainly

`on=23` at peak. The goals' own scale: a run that reaches gameplay reads `on=`
in the 148–453 band; the attract loop reads 4–12. **23 is neither** — past the
attract loop, nowhere near gameplay. 23 top inserts against the 22:40 session's
212.

So this is the music death reproduced in an *early* state, not in a mission.
The delivery result is solid because it is a ratio and not a rate: ten idle,
ten delivered, none missed. The cycle result is solid as a refutation of
necessity. Nothing here characterises the storm at full gameplay volume.

## What the two sessions say together

| | 17 Sep 22:40 | 18 Sep 09:00 |
|---|---|---|
| top inserts | 212 | 23 |
| `walks_with_a_cycle` | 88 | 0 |
| `[APU-WALKCAP] hit` | 87 | 0 |
| music died | yes | yes |
| `cycle_break` | off | on, never fired |
| idle delivered | not instrumented | **10 of 10** |

Cycles in one, none in the other, music dead in both.
