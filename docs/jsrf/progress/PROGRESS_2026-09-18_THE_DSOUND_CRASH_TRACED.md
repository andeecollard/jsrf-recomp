# The DSOUND crash, traced to the instruction — 18 September 2026

The crash that has been destroying half of every measurement. Not fixed. But
the mechanism is now pinned to a single instruction, two hypotheses are dead
with evidence, and the one value that would settle it is identified along with
why the APU model cannot currently see it.

## The fault, register by register

    HOST PC   sub_001A2E2E +0x670        guest fault 0xFFFFFFBE
    GUEST     EAX=FFFFFFB4 ECX=000006C4 ESI=00000000 EDI=00000000

`recomp_0008.c`, `sub_001A2E2E`:

    eax = MEM32(esi + 0x50);                   // ESI=0 -> reads page 0 -> 0
    ecx = MEM32(esi + 8) + ecx * 8 + 0x6C4;    // -> 0x6C4, matches ECX
    if (ecx != eax) goto loc_001A2E8B;
    loc_001A2E8B:
      edi = ZX8(MEM8(eax + 0x18));             // guest 0x18 -> 0, matches EDI
      eax = eax + 0xFFFFFFB4u;                 // 0 - 0x4C, matches EAX
      eax = ZX16(MEM16(eax + edi*2 + 0xA));    // 0xFFFFFFBE  <- THE FAULT

Every reported register is reproduced by that path and no other. The chain is
already documented at `apu_vp.c:454`:

    001A25AA  reads FECTL/FEDECMETH/FEDECPARAM; dispatches if trapped
    001A24BE  if (method != 0x8000) return
    001A241F  if (h >= 0x100) return; if (this->+0x2C0) return; PERSIST check
    001A200D  pBuf = this->owner[h] (+0x2C4 + h*4);   NO NULL CHECK
    001A2E2E  pBuf->...  -- the fault, with pBuf == NULL

**ESI == 0 is `owner[h]` being NULL.** Page 0 is ordinary mapped RAM here
(`RECOMP_TRAP_NULL` is not set), so the NULL does not fault where it is read —
it faults 0x4C bytes earlier, several loads later, which is why this looked
like an ESI bug for so long.

## Dead hypothesis 1: the voice-lock race

`apu_vp.c:658` states its own decision rule: *"a single faulting run either
shows the fatal raise flagged L — mechanism proven, guard proven sufficient —
or does not, and kills this hypothesis outright."*

Across every crash on 18 Sep, the last raise before the fault is:

    LAST RAISE: 3D:v0[]<-TVL          flags EMPTY, not L
    locked_raises=0

Eight runs today, on top of the 18 previously recorded. **It does not.**
`RECOMP_APU_IDLE_TRAP_LOCK_GUARD` would not have prevented one of them and
should stay off.

## Dead hypothesis 2: ownership by voice state

The ISR is entitled to dereference `owner[h]` because *"on hardware a handle
only reaches this chain if DirectSound put the voice in a list, which it does
after it has an owner for it"*. Our list and the guest's ownership have
measurably diverged, so: do not raise for a voice the guest no longer owns.

Built as a counter, not a guard — `g_apu_voice_owned_now`, set at VOICE_ON,
cleared at retirement, with an `O` flag in the trap ring. Three runs:

| run | result | what it proved |
|---|---|---|
| 1 | `0 owned, 0 UNOWNED`, 4 raises, crashed | instrument never printed at the fault |
| 2 | `15987 owned, 0 UNOWNED`, `release=0` | clear side never exercised |
| 3 | `0 owned, 15524 UNOWNED`, `off=301`, **no crash** | proxy does not discriminate |

Run 3 is the one that kills it. If "unowned" meant `owner[h] == NULL`, a run
with **15,524** unowned raises would have crashed 15,524 times. It crashed
none. So `VOICE_OFF` is not what clears the guest's `owner[]` — the same
objection that applies to `VOICE_RELEASE`, which I should have applied to both
at once.

**What the proxy does measure, and it is worth keeping:** 100% of idle-trap
raises target a voice the guest has already turned off. That is a clean
characterisation of when the trap fires. It is simply not the crash signal.

## Two instrument failures, both reading a confident zero

Worth recording because they are the same failure in two costumes, and both
would have produced a false refutation if believed:

1. **Printed where the crash path does not look.** The counter sat beside
   `[APU-VOICE2]`, in a function the fault handler never calls. The last value
   on disk was a periodic report taken before any raise had happened. It read
   `0 owned, 0 UNOWNED` on a run with four raises and a fault.
2. **Keyed on an opcode this title never sends.** It cleared on VOICE_RELEASE.
   The same report read `on=288 off=258 release=0` — VOICE_RELEASE fired zero
   times all run, so the clear side was never exercised and `UNOWNED=0` was
   silence, not an answer.

Both were caught only by asking for the positive control before writing the
conclusion down. An instrument that is not watching reads zero, and zero looks
like data.

## Why no APU-side signal can settle this

`owner[]` is DirectSound's own table at `this + 0x2C4 + h*4`. It is populated
when a buffer is **created** and cleared when it is **released** — DirectSound
API-level events, inside the title's own statically-linked XDK code. The APU
register stream sees `VOICE_ON`, `VOICE_OFF`, `VOICE_RELEASE` and method
traffic. **None of those is buffer lifetime.** No combination of them
reconstructs `owner[]`, which is why three runs of trying produced a metric
that is 0% or 100% and never discriminating.

## The one route to a real fix

Read the guest's table. That needs `this`, and `this` is obtainable: the ISR
chain is guest code **we generate**, so a probe installed into a *copy* of the
gen tree at `001A241F` or `001A200D` captures it once. After that the model can
read `owner[h]` directly before every raise and decline to hand over a handle
the guest will dereference into NULL.

That is a fix rather than a guard — it makes our raises honour the invariant
the ISR is entitled to rely on — and it is the established pattern here:
instrumentation installed into a gen copy by a script in
`diagnostics/jsrf_first_fault/`, never committed into a fix.

**Cost, stated honestly:** a gen copy (cheap with APFS `cp -c`), a rebuild of
the handful of affected translation units, and one run. The risk is that
`this` is per-device and the probe has to catch the right one.
