# Goals — 13 Sep 2026: the tutorial is the last gate, and it is one question wide

Supersedes `JSRF_GOALS_2026-09-12_CAN_WE_DO_IT.md` on ordering. Its framing was
"one gate (the tutorial jump) and a renderer residency step"; the gate is now
located to two predicates and the black screen that stood in front of it is
gone.

## What changed today

The black screen on New Game was **ours**, not the title's. Nothing ever set
`m_bFatal`; `CActMan::Idle` was looping on a code address because a switch
dispatcher's arms were never translated and the arm's `pop esi` never ran.
Three translator fixes later the unattended path reaches the Corn tutorial in
~46 s, `m_bFatal` stays 0, and `JSRF_ABI_CHECK_ALL` over every chunk reports
four violations, all of them the expected CRT signatures.

That matters for planning more than the bug did: **measurement is now cheap**.
Reaching the tutorial cost a human playthrough for the whole of last week. It
now costs 46 seconds and no person, which is why three separate instruments
could be built, rejected and replaced inside one afternoon.

## G1 — Decide which end of the tutorial stall to work from  (blocking, one run per side)

Everything else waits on this, and it is a single comparison.

Both CPlayers park at `+0xE50` = 23 and 25, inside the `0x14..0x1A` band that
`sub_00080340` treats as externally driven, and `[[this+0x2D0]+0x38]` is 0, so
the pose writes are skipped. Ask xemu what it has at the same moment:

    diagnostics/jsrf_first_fault/xemu_cplayer_state.py

* **xemu also in `0x14..0x1A`** → the state is right; the fault is in what the
  handler does. Descend from `sub_000A76E0` (the virtual that writes both
  parked values, `.rdata` slot `0x001CD574`).
* **xemu outside the band** → the state was chosen wrongly; the `0x09`/`0x0A`
  setter family is where it should have gone.

Cost: one xemu playthrough to the tutorial, one scripted run on our side.
**This is the only item here that needs a person.**

## G2 — Close the Draw question  (small, and it removes an inference)

Whether Draw is called for Corn is still unmeasured — it needs a second walker
hook and the one-probe discipline ruled it out. The vector-add reading makes a
pure render fault unlikely, but "unlikely" is an inference and this project has
been wrong that way before. One cold hook on the draw walker settles it.

## G3 — The 154 remaining truncated switches  (background, not blocking)

Down from 276. `JSRF_ABI_CHECK_ALL` says none of them clobbers anything on the
path to the tutorial, so **do not expect fixing them to move the tutorial**.
They are correctness debt worth paying when the gate is open, not before. The
remaining shape is harder than the ones fixed: a genuine function start sits
between the dispatch and its table, so extending would cross real code.

## G4 — Name the recomp as we go  (carried forward from 12 Sep, unchanged)

The middle path still holds: finish the recomp, name it as we go, do not start
a second matching decomp. Today added `CActSequence`'s 64 states, CPlayer's
vtable layout and the `0x14..0x1A` setter family to what is named.

## What is explicitly NOT a goal

* **Revisiting ABI/register clobber.** Armed across every chunk, the tutorial
  path is clean. New evidence would have to point back there.
* **Alias extents, input scheduling, thread suspension.** Settled, committed,
  and not implicated.
* **Heavier instrumentation.** Two instruments were abandoned today for
  perturbing the guest — `jsrf_func_arg`'s 384-entry linear scan at hot sites,
  and `RECOMP_OBJECT_DUMP`'s file-per-report I/O. Every measurement now needs a
  control run against the untouched binary first, and gets rejected if
  progression to the tutorial differs. Three were.

## The order, and why

G1 first because it is blocking and cheap, and because it decides whether G2
matters at all. G2 second because it is small and removes the one inference
still load-bearing. G3 whenever the gate is open. G4 continuously.
