# 21 September 2026 — the lock did not span the critical section

A review of the morning handover's ADX fix found the fix could not work, and
five more items alongside it. All six were worked. Two of them changed what is
believed about a subsystem, and those are sections 4 and 5 — read those first
if you read nothing else.

Supersedes the ADX and RECOMP_TEXMODE_APPROX sections of
`handovers/HANDOVER_2026-09-21_MORNING_THE_LOCK_WAS_A_PRIORITY_AND_WE_NEVER_ENFORCED_IT.txt`.

---

## 1. THE ADX FIX DID NOT RESTORE EXCLUSION, AND THE 430 s RUN DID NOT SHOW IT DID

The morning handover named the freeze correctly: CRI's ADX lock is mutual
exclusion built on priority elevation, and this runtime records priority
without enacting it. The mechanism, the measurement (`lock_count=-32680066`
against `queries=32680727`) and the eight-call-site proof that the narrow fix
is the complete fix all stand.

**The fix did not follow from it.** It took a recursive mutex at the top of
each body and dropped it at the bottom. Read the handover's own interleave
again:

    A locks    count 0->1, A raised to 16, A's real priority saved
    B locks    count 1->2, the `jne` skips, B untouched
    A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
    B unlocks  count 1->0, restores A's saved value onto B
    A locks    A is already at 16, so it saves 15.  POISONED.

Five complete calls, already in a serial order. A mutex held only inside each
body permits that order unchanged. What the elevation prevented was **B
entering the region while A is inside it**, which is a statement about the span
between A's lock and A's unlock, not about either body.

The 430 s confirming run is consistent with the fix working and equally
consistent with the interleave simply not occurring — nothing in it
distinguishes the two.

**Now:** `diagnostics/jsrf_first_fault/adx_guard.c`. The guard is taken by the
guest lock and released by the matching guest unlock, with the guest's own work
in between. Recursive (the guest nests — measured depth 2), owner-tracked (the
spin at `sub_001437B0` issues unlocks it never matched with a lock, and
releasing on those would hand another thread's region away), and bounded (a
lock never unlocked would otherwise wedge every other guest thread with no
diagnosis; after a timeout the waiter takes it anyway and counts a steal).

`jsrf_adx_guard_{off,zero,on}` drives the five steps from a third thread with
the guest's two words and XAPI's 15↔16 mapping modelled exactly. The `off` arm
is the positive control and **must** poison; the `on` arm asserts B *cannot
enter*. Run against the previous implementation while it was written, the `on`
arm fails in under a second with `saved==15` and B's priority 9 overwritten
with A's 8.

## 2. `=0` TURNED BOTH NEW SWITCHES ON

`RECOMP_TEXMODE_APPROX` and `RECOMP_ADX_SERIALIZE` were `getenv(...) != NULL`.
The A/B the handover asked for — "run with `RECOMP_TEXMODE_APPROX=0`; if the
flicker persists it is not mine" — is exactly the arm a presence test cannot
express. `recomp_switch.h` exists because that has cost three conclusions here.

Both now go through the helper. The `[COMBINER]` switch state moved **before**
the census's early-out, because the `=0` arm can legitimately refuse nothing
and approximate nothing, at which point every line was suppressed and the run
could not say which arm it was.

`jsrf_switch_audit` is **green for the first time since ee31f41** — the ratchet
was ten over baseline at HEAD before this session, not because of the morning's
work. Two more hand-rolled booleans converted (`RECOMP_GLYPH_GPU_DIFF`, and
`RECOMP_THREAD_PRIORITY`, which accepted only the literal `"1"` so `=on` and
`=yes` silently took the control arm), eight value-carrying switches exempted
by name with reasons.

## 3. THE FLIP SNAPSHOT WAS ONE BUFFER WITH FOUR READERS

`snapshot_surface()` admitted the consequence and called it acceptable: "The
copy races the reader and can tear a frame". A tear was not the whole cost —
the same function **reallocs** that buffer when surface geometry changes, and
this title flips between surfaces of differing size, so a reader mid-`memcpy`
could be reading freed storage. Its dimensions arrived as four separate loads,
so it could also pair a new frame's width with an old frame's height.

`src/kernel/frame_pool.h` publishes pixels, dimensions, surface identity and
sequence as one object, and reuses a slot only once no reader holds it.
Neither side blocks. `nv2a_pb_exec_surface()` keeps its pointer-returning
contract through one held frame per calling thread — asking for the next frame
is what gives the last one back.

`jsrf_frame_pool` runs a producer flat out against three readers and checks
every frame for pixels from more than one publish. Against the single-buffer
version: **90,647 torn frames of 90,790** and a dimension mismatch, in well
under a second. Against the pool: zero.

Five slots, and `[FLIP-SNAP]` says whether that was enough, because a publish
with no free slot keeps the previous frame on screen and looks exactly like a
title that drew nothing — a reading this file has already had to retire once.
Completed 60 s run: `3979 frames published, 0 DROPPED`.

The pool's lazy init was itself racy on the one call that matters — producer in
`frame_pool_begin` and presenter in `frame_pool_acquire`, either of which could
run it. The static pool carries its initial state instead (`FRAME_POOL_INIT`).

## 4. THE PRESENT PATH, MEASURED — AND IT IS NOT WHERE THE HANDOVER SAID

The handover wrote: *"Every flip waits for the GPU and round-trips the whole
colour surface through guest RAM. That is where the judder is."* Three distinct
costs are named in that sentence and only two had ever been timed apart. The
third — this file's own `memcpy` at FLIP_STALL — was inside `rest`, and
presentation was not timed at all.

All four are now separate. From a completed 90 s silenced run (`intro.pad`,
`smoke-reviewfixes`):

| cost | where | measured |
|---|---|---|
| drain (waiting for the GPU) | `[METAL]` | **3141.4 ms** over the run |
| readback (GPU → guest RAM) | `[METAL]` | **2010.8 ms** over the run |
| snap (guest RAM → presented copy) | `[STAGE] snap=` | **0.02 ms/frame** |
| convert + upload + draw-submit | `[PRESENT]` | 0.09 + 0.22 + 0.05 ms/frame |

Reproduced on a second completed run (`smoke-poolcheck`, 60 s, same pad):
`snap=0.02 ms`, `convert 0.09 / upload 0.22 / draw-submit 0.04 ms`. The two
runs agree to the printed precision on every term.

**The copy everybody blamed is 0.02 ms a frame.** It is roughly one part in
four hundred of `[STAGE] sync=7.4 ms`. The CPU-side present work is another
0.31 ms. The cost is the drain and the readback, and **the drain is the larger
half.**

That changes the design question. Presenting a completed GPU texture directly
removes the readback, the snap and the convert+upload — and **keeps the drain**
unless presentation is also made asynchronous. On these numbers that is at most
about 39% of the sync cost, not the whole of it. Size the work against that
before spending a week on it, and re-measure on a gameplay scene: this is the
intro, and `[STAGE] sync` is one call per frame while `[METAL]` counts 10,333
sync calls from `swap` alone.

`no_flip_sync=1` is not the alternative. Its own comment says the presenter
then reads stale guest RAM; it is an upper bound on the saving.

## 5. THE SEVEN DROPPED AUDIO METHODS — NOW WITH EVIDENCE, AND IT POINTS AWAY

`[APU-FE-UNKNOWN] dropped=69 distinct=7: 02A4 02B0 02A0 02A8 02AC 0350 0370`
names the gate, not the input. Seven numbers cannot say whether one method
arrived 63 times and six arrived once, what they carry, or when.

The census now records, per method, the count, the first and last argument
(and whether it ever changed), the current voice at each, and seconds into the
run. From the same completed run:

    02A4  x1   arg 00000FFF == 00000FFF  voice 0..0    t 0.0..0.0 s
    02B0  x1   arg 00000FFF == 00000FFF  voice 0..0    t 0.0..0.0 s
    02A0  x1   arg 00000FFF == 00000FFF  voice 0..0    t 0.0..0.0 s
    02A8  x1   arg 00000FFF == 00000FFF  voice 0..0    t 0.0..0.0 s
    02AC  x1   arg 00000FFF == 00000FFF  voice 0..0    t 0.0..0.0 s
    0350  x5   arg 00000000 == 00000000  voice 64..68  t 0.0..13.3 s
    0370  x5   arg 00000000 == 00000000  voice 64..68  t 0.0..13.3 s

They are **two groups, not seven of a kind**:

- `0x02A0`–`0x02B0`: five consecutive dwords, **once each, all at t=0.0, all
  carrying the same constant `0x00000FFF`, at voice 0.** One-time
  initialisation, before any voice exists. These cannot chop a vocal.
- `0x0350` and `0x0370`: five each, argument **always zero**, tracking voices
  64..68 as they are set up. `0x0350` falls inside the `SET_VOICE_SSL_A`
  (0x320) .. `SSL_B` (0x35C) block; `0x0370` in the `LFO_ENV` (0x36C) ..
  `TAR_FCA` (0x374) gap.

**Fifteen drops, ten of them a zero written once per voice.** The handover's
reading — "Dropped voice commands would chop a vocal and silence a stream,
which fits both symptoms" — is not supported by this. A non-empty output ring
rules out underrun; a non-zero drop count rules nothing in. Nothing yet ties a
specific drop to a specific silence, and what would is a run where the voice
carrying the missing stream is named and one of these methods is seen carrying
that voice number.

**AND THE DECODE IS NOT A REGISTER-TABLE JOB.** "Seven specific opcodes,
findable statically" assumed the definitions exist somewhere. Checked against
xemu's own `hw/xbox/mcpx/apu/apu_regs.h`, which is where `src/apu/apu_regs.h`'s
map came from: **the reference does not define these seven either.** The two
maps agree method for method. The decode has to come from the guest side —
which XDK DirectSound routine emits each one, read out of the XBE.

### 5a. AND THE GUEST SIDE ANSWERED IT

The trap handler already leaves the faulting store's host PC in
`g_apu_trap_host_pc`, and `apu_core.c` already had a `dladdr` resolver for it
that only one trace used. Wiring that into the census names the emitter, and
reading the emitter's other stores puts each unknown method in the company it
keeps. One 60 s run:

    02A0 02A4 02A8 02AC 02B0   written by sub_001A5671
    0350 0370                  written by sub_001A368A

**`sub_001A368A` is inside `DSOUND::CMcpxVoiceClient::SetFilter`**
(0x001A332D..0x001A3804, bracketed on both sides by XbSymbolDatabase symbols).
It commits a whole voice, in method order, and the two unknowns are two slots
in an otherwise fully-named block:

    2F8 SET_CURRENT_VOICE     360 TAR_VOLA
    300 CFG_VBIN              364 TAR_VOLB
    304 CFG_FMT               368 TAR_VOLC
    308 CFG_ENV0              36C LFO_ENV
    30C CFG_ENVA          ->  370 <-- UNKNOWN
    310 CFG_ENV1              374 TAR_FCA
    314 CFG_ENVF              378 TAR_FCB
    318 CFG_MISC              37C TAR_PITCH
    31C TAR_HRTF
->  350 <-- UNKNOWN

Eighteen per-voice registers; we model sixteen. **`0x370` is a single
four-byte hole in an otherwise complete `SET_VOICE_TAR_*` run**, so it is very
likely the missing member of that series; `0x350` falls inside the SSL_A
(0x320) .. SSL_B (0x35C) block. Neither is confirmed — the company they keep
is evidence, not a definition.

**`sub_001A5671` is a one-time DSOUND APU bring-up.** It loads the main-region
mixbin registers 0x202C/0x2030/0x2034/0x2038/0x203C from a table, then writes
the **literal immediate `0xFFF`** to all five of 0x2A4/0x2B0/0x2A0/0x2A8/0x2AC,
then `SET_CURRENT_INBUF_SGE` (0x804) ← `0x7FF`, then `SET_HRTF_SUBMIXES`
(0x2C0). Five registers set to an all-ones default at init, beside a 0x7FF
sibling. (XbSymbolDatabase has no symbol covering 0x1A5671 — the nearest below
is `CMcpxBuffer_SetBufferData` at 0x1A4B11 and the next is 0x1BBCA5, a gap of
0x17000, so the routine is **not** attributable by name.)

**So they are not why the music is missing.** Five are init-time writes of a
constant, before any voice exists. The other two are a zero written once per
voice by a routine that successfully delivers the other sixteen registers of
the same voice. The handover's "Dropped voice commands would chop a vocal and
silence a stream, which fits both symptoms" does not survive knowing what they
are.

`[ADX-RATE]` "printed nothing at all in either session" was also not a finding.
The instrument is opt-in and `RECOMP_ADX_RATE` was not set. It now says so once
when it is off, because an absent report and an absent server pass look
identical in a log.

## 6. THE CONTROL-FLOW GATE NOW HAS ADDRESSES

The gate failed on `switch_resolved 487 -> 481` while every unresolved measure
improved, and was re-baselined. The handover was right that it is one-sided,
and right not to change a check's semantics in the same pass. A **ratio** would
not have fixed it: it would equally have passed six sites genuinely going
unresolved while six duplicates vanished.

The identity of a site is its **guest address**, which survives re-carving:

- switch sites — the jump table's address, with how many dispatches reached it
  resolved and how many unresolved. Fails when a table's unresolved count
  **rises**. (449 tables recorded; all 481 resolved dispatches are table-shaped,
  so the two regexes see every switch site the tree has.)
- todo sites — the address in `RECOMP_UNIMPL(..., 0xVA)` and its mnemonic.
  Fails on a new **address**. (107 addresses over 123 comments; the rest are
  the same instruction in duplicate bodies.)
- stub sites — the `sub_XXXXXXXX` names. Fails on a new one.

Disappearing sites and falling counts are reported, never failed. A
`switch_resolved` fall is now **explained** rather than failed when no table
gained an unresolved dispatch — which is exactly the 487→481 case. Verified
both ways against perturbed baselines: a resolved→unresolved table, a new TODO
address and a new stub each fail with the address named; a bare total fall with
no site regression passes with the explanation printed.

**`todo 122 -> 123` cannot be attributed now.** The gen that produced the 122
baseline (`git_head 3b9d2fc`, generated 2026-09-20T12:52:08Z) was overwritten
in place by the 21 Sep regeneration, and neither its `functions.json` nor the
icall dump it was built from survives. `gen.pre-merge-20260919-KEEP` also reads
122 but all of them are `todo_silent`, so it is a different 122. What can be
said: all 123 carry `RECOMP_UNIMPL`, so none is silent, and two preserved
20 Sep runs contain **zero `[UNIMPL]` lines** — no untranslated instruction was
reached. The baseline now records the addresses, so the next regeneration names
the new one instead of reporting a number.

---

## 7. A SECOND BUG, FOUND AND FIXED: THE D3D11 COMBINER READ CD AND AB SWAPPED

Two handovers wrote this down and neither resolved it — "`d3d8_combiners.c:169`
reads the AB/CD destination bits inverted relative to `nv2a_texture_copy.c`.
Still unfixed, Windows only."

Checked against the authority both files name, xemu's `parse_combiner_output`:

    out->cd = value & 0xF;          out->ab = (value >> 4) & 0xF;

**CD is the low nibble.** `nv2a_texture_copy.c` was right; `d3d8_combiners.c`
had them swapped and also called bit 18 "an unused alias, same as mux_sum" —
it is CD blue-to-alpha, and bit 19 (AB blue-to-alpha) was missing entirely.

**What it cost.** JSRF's graffiti shader emits colour output word `0x000820D0`,
counted 24,821 times in one player session. Correctly: `AB → R1`, `AB_DOT`,
`AB_BLUE_TO_ALPHA`. Inverted: `ab_dst = 0`, which is `NV2A_REG_ZERO` — and the
HLSL emitter treats ZERO as *discard*. So the tag's dot product was **not
written at all**, and the unused CD product went to R1 in its place. The same
shader was fixed on the NV2A path in 777f04a ("The tags paint"); on Windows it
was still broken, for this reason.

That is also why it survived: the inverted read did not produce a wrong colour,
it produced a **missing write**, and a register left at its initial value still
shades something plausible. A wrong picture gets reported; a missing write gets
lived with.

**Why no test caught it.** `d3d8_combiners.c` is built only on Windows and its
own header includes `<d3d11.h>`, so nothing in the suite could reach its copy
of the shifts. The layout now lives in `src/d3d/d3d8_combiner_bits.h` — pure,
no includes beyond `stdint.h` — and **both paths call it**, so they cannot
drift again. `jsrf_combiner_output_word` pins every field against xemu's values
and against the real graffiti word, on any host; run against the pre-fix decode
it fails ten assertions including "ab_dst decoded as ZERO".

Also fixed while there: the alpha output word was parsed with the RGB parser,
so bits 12/13/18/19 were read as dot and blue-to-alpha flags on a channel that
has neither. Harmless only because nothing downstream read them; it now parses
through `nv2a_parse_alpha_output_word`, which zeroes them by construction.

Blue-to-alpha is now emitted on the D3D11 path, **after** the alpha block —
`nv2a_texture_copy.c` records paying for that ordering once already, because
emitting it earlier lets the alpha combiner overwrite the value the flag exists
to deliver. The carriers are named per stage; a single name would be redeclared
by the second blue-to-alpha stage, and the tag shader is the title's only
eight-stage program.

`src/d3d/d3d8_combiners.c` does not compile on macOS, so it was syntax-checked
with `x86_64-w64-mingw32-gcc` (clean). **The emitted HLSL has not been run** —
that needs a Windows session.

---

## 8. HOUSEKEEPING: THE STALENESS GUARD REFUSED EVERY macOS RUN

Fixing section 7 made `run_common.sh` refuse to start: `d3d8_combiners.c` was
newer than the binary. It is inside `if(WIN32)` in `src/d3d/CMakeLists.txt`, so
macOS never compiles it and it is provably not in that binary.

This is the second time this guard has fired on a file that cannot make the
binary stale — its own comment records the first ("also matched `*_test.c` ...
A check that fires when nothing is wrong is one people switch off"). It now
asks the BUILD which sources were compiled, by looking for an object named
after each one, rather than carrying a list that would go stale when the
platform split moves. Headers are still always counted: a header can reach
anything and has no object of its own. An empty object set falls back to
counting everything, because a guard that fails open is worse than one that
fires too often.

Checked both ways: touching `d3d8_combiners.c` now passes, touching
`src/kernel/nv2a_pb_exec.c` still refuses and names it.

One shell trap worth recording, because the error message points at the wrong
line. The loop lives inside `$( )`, and a `case` pattern's unbalanced `)`
closes the command substitution early — `syntax error near unexpected token
';;'`, on a line that is fine. The patterns carry a leading `(` so the parens
balance, which POSIX allows for exactly this reason.

---

## 9. HOUSEKEEPING: A SWEEP FOR CONSTANTS THAT DISAGREE

Section 7's bug was one file's copy of a bit layout disagreeing with another's.
That is a searchable shape, so the whole tree was swept for it: every
`#define NAME <integer>` in `src/`, grouped by name, flagged where two files
give different values.

**One hit, and it is a landmine rather than a live bug.**
`src/nv2a/nv2a_pb_test.c` carried

    #ifndef NV097_SET_BEGIN_END_OP_TRIANGLES
    #define NV097_SET_BEGIN_END_OP_TRIANGLES 0x04
    #endif

under the heading "Supplemental defines not in the register file". It *is* in
the register file, as **0x05**; 0x04 is `LINE_STRIP`. The fallback never fired
only because `nv2a_regs.h` is included four lines above it, so the guard was
always false. Reorder that include, or drop the name from the register file,
and the test would quietly draw a line strip while the comment beside it said
"triangle list". Deleted rather than corrected — a fallback for a name that is
always defined has no upside.

The 112 `-Wmacro-redefined` warnings in a macOS build were checked the same
way rather than assumed benign: of the 20 NV097 constants `nv2a_pb_exec.c`
redefines against `nv2a_regs.h`, **zero differ in value**. They warn on
textual difference (`0x0200` against `0x00000200`), which is exactly the noise
a real mismatch would have hidden in.

Two defects of my own, found reviewing this session's new code:

- `frame_pool_publish` trusted its caller's dimensions. Passing it a size
  `frame_pool_begin` was not asked to allocate would publish a frame that
  reads past its own storage. It now refuses and counts the drop.
- `adx_guard`'s `stolen_from` counter was collected and printed nowhere. It
  rides on the steal line now; a counter nothing can read is one this tree has
  had to retire before.

---

## 10. THE SYMBOL TABLE, USED AS A CHECKER — AND WHAT IT FOUND

`symbolize.py check` reads 1,347 named functions, 1,333 of them exactly on our
function starts. The 14 that disagree, and two sweeps built on the 1,333, were
worked. **Three hypotheses died and one held**, so read the dead ones too — they
are cheaper to not re-run than to re-derive.

### DIED: the one-byte boundary disagreement is dormant

`0x00051490 CMissionChild3Child2::Exec1Event` sits inside our `sub_0005148F` —
one byte. Our function begins on a padding `nop`, so the real entry is one byte
in and nothing defines `sub_00051490`. That would matter if anything dispatched
to 0x51490; **nothing references it anywhere in the generated tree**, so it is
inert. Twelve of the other thirteen are `*_handler` entries at 0x00186xxx-
0x00187xxx, which are SEH handler thunks and legitimately interior.

### DIED: over-long extents are not inflating `coverage_bytes`

100 of the 1,333 matched functions have an extent that runs past the *next*
named function's start — a whole run of `CActSequence::*Tutorial`/`*TestRun`
entries all claim to end at 0x0007D5D0. But clipping every extent at the next
named start moves `coverage_bytes` by **371 bytes of 1,615,561** (0.02%),
because the overlaps cover bytes the neighbours' own extents already cover and
the union absorbs them. The gate's headline number is not resting on this.

### DIED: "reuse the resolved table" is not 72 easy wins

`audit_unresolved_flags.py` reports 2 **reachable** dead-fallback branches, both
in `sub_001237A0`. Reading them: `adc al,[eax]` chains, `jg` after `adc`, a
store to `0x64001238`. That is the data-swept-as-code signature the audit's own
header describes, in the tail of a long function — not a live mis-compiled
branch. (Its `--list` printed every site without marking which were
reachable, so the two the ratchet actually gates on could not be found from its
output at all — a failing run printed 90 lines and named none of the 2 it
failed on. `--list` marks them `REACHABLE` now, greppable for the same reason
`UNRESOLVED FLAGS` is.)

### HELD: half the unresolved switch dispatches already know their answer

The decomp names 9 jump-table data rows with **exact element counts**. Eight we
already resolve. The ninth —

    0x0007C9C0  void *[5]  CActSequence::ReturnFromFullRoboyMenu__jumptargets
    0x0007C9D4  byte[12]   CActSequence::ReturnFromFullRoboyMenu__jumptable

— is one of our 53 unresolved tables. Read out of the XBE it holds exactly five
targets (0x7C8C1, 0x7C99A, 0x7C96A, 0x7C982, 0x7C9B5) and a 12-byte index
`[0,4,1,2,4,4,4,4,3,3,3,3]`: a two-level MSVC switch.

**Four functions dispatch through that one table** — `sub_0007C600`, `C720`,
`C7E0`, `C800` — and exactly one of them resolves it. The one that resolves it
reports "switch: 5 entries, 5 targets", matching the decomp exactly.

Generalised from the site record the gate now keeps:

    unresolved dispatches                    142
      through a table resolved elsewhere      72   in 32 tables
      through a table resolved nowhere        70   in 21 tables

**It is not 72 easy wins, and checking that is the point.** The site that
resolves 0x0007C9C0 is the one whose body *contains* the arms; for the other
three the arms are labels inside somebody else's function, and four of the five
have no body of their own. So the blocker is `switch_arms_no_body` (445), not
the table lookup, and what each arm needs is an **entry point**.

**And the tool for that already exists and never sees them.**
`recover_midfunction_entries.py` — run by `regenerate.sh` — builds exactly this:
"an address that lies at an instruction boundary inside an already-detected
function is real code, and translating it from there to the end of the function
that owns it produces the body it should have had". It operates on STUBS,
i.e. addresses something statically calls. An arm reached only through an
unresolved indirect table never becomes a stub, so the recovery pass never sees
it. The arm walk knows the addresses (`switch_arms_tight = 522`); the recovery
pass knows how to build bodies for them; nothing joins the two.

That is the next piece of work, and it is plumbing between two existing tools
rather than new analysis.

**HOW MUCH THIS COSTS TODAY: nothing measurable yet.** `RECOMP_ITAIL`'s failure
path is `g_esp += 4; g_eax = 0` — it eats the return address, which is the
stack-corruption shape G20 was. But `recomp_itail_fail_log` prints on the first
hit at each site (powers-of-ten cadence, and 1 prints), and **three long runs
contain zero `[ITAIL]` lines**. No unresolved tail jump has been taken in any
run on record. This is a latent risk with a known size, not a live defect.

The split is printed by `control_flow_gate.py` now, so the next session gets
the number without redoing the analysis. It is derived from the site record
already in the baseline and needs nothing from the decomp — that table is read
in place for verification and never vendored.

---

## 11. A SANITIZER AND A CROSS-COMPILER, AND WHAT THEY FOUND

Three tools nothing in this tree routinely runs: ThreadSanitizer on the
concurrency code, `x86_64-w64-mingw32-gcc` over all of `src/`, and
AddressSanitizer + UndefinedBehaviorSanitizer over the whole suite.

### THE WINDOWS BUILD HAS NOT LINKED SINCE 4537424

`kernel_thread.c` calls `GetThreadPriorityXboxExact` and
`SetThreadPriorityXboxExact` unconditionally, at three sites. Both are
**defined** only in `src/platform/win32_compat.c` and **declared** only inside
that header's `#if !defined(_WIN32)`. On Windows `src/platform/CMakeLists.txt`
makes `platform` an INTERFACE library — `win32_compat.c` is not compiled at
all. So on Windows the two are implicitly declared (mingw calls this an error)
and then have nothing to link against.

They are not Win32 API. They are ours, added by 4537424 ("The guest sets base
priority 16 and could only ever read back 15") the day before, which is when
this broke.

Falling back to the real Win32 priority API is not an option: that is the
bucketing the commit exists to escape, and `kernel_thread.c`'s own comment
records what it costs — a three-call cycle run 64.5 million times while the
guest polls for a value the bucket can never return. So Windows gets the same
exact-value contract, keyed by thread id because Windows hands us an OS handle
rather than this layer's own object, with the same "an unnameable handle is
counted and dropped, never charged to the caller" rule.

**Two caveats, stated rather than left to be discovered.** Nothing retires an
entry when a thread exits, so a reused thread id inherits a stale exact
priority — the POSIX side cannot have this because its store dies with the
object, and a title that churns threads would need the table keyed on
something that retires. And it is **untested at runtime**: written on macOS,
syntax-checked with mingw, no Windows session has run it.

### SIGNED SHIFT OVERFLOW: 13 SITES

UBSan caught one at runtime — `apu_vp.c:5319`, *left shift of 255 by 24 places
cannot be represented in type 'int'* — which is `NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE`,
`(0xFF << 24)`. Sweeping statically for the same shape found 13:

    apu_regs.h    (0x3 << 30) (0xFF << 24) x3 (0xF << 28)
    nv2a_regs.h   (1 << 31) x8

Every one is a signed `int` literal shifted until it overflows. They produce
the intended bit pattern on every compiler anyone has used, and they are still
UB the optimiser is entitled to act on. The literals are unsigned now; the
values are unchanged.

### A HAND-ROLLED `offsetof` THROUGH A NULL POINTER

`kernel_file.c` computed a header size as
`(ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName`. UBSan: *member
access within null pointer*. The same file already uses real `offsetof` 700
lines earlier for the same structure and the same field, so this was an
inconsistency inside one file, not a house style. Replaced; no other
hand-rolled instance exists in the tree.

### THE CONCURRENCY CODE, UNDER TSan — INCLUDING MY OWN TEST

`frame_pool_test` and all three `adx_guard_test` arms are TSan-clean. That
result is only worth having because TSan was shown to catch the thing it is
looking for: run against the single-buffer producer the pool replaced, it
reports the race immediately.

It also found a defect in **this session's own test**: `frame_pool_test` used a
`volatile int` stop flag, and `volatile` orders nothing between threads. A test
whose harness races cannot report cleanly on anything; it is `atomic_int` now.
Its positive control was worse — a fixed 4,000 frames left the readers with 70
under TSan, below the "the race was never exercised" floor, so the test failed
for being too fast rather than for being wrong. The producer now publishes
until the readers have actually seen enough, which makes the control
independent of how the two sides are scheduled.

### CLEAN, AND WHAT THAT COST TO ESTABLISH

After the two fixes, the whole suite is clean under
`-fsanitize=address,undefined`: **0 findings across 105 passing tests**.

That number was nearly reported wrong. `ctest --output-on-failure` prints only
FAILING tests' output, so a sanitizer finding in a test that still passes is
invisible — the first "zero findings" reading was an artefact of the flag.
Re-run with `-V`, it was 62. Every one was the same class: UBSan's `alignment`
check on the kernel-supplied `ucontext_t` in `mcpx_trap_handler`. macOS
declares that type 16-byte-aligned for the NEON state it reaches and hands the
handler an 8-byte-aligned frame; there is no `sigaltstack` and no `SA_ONSTACK`
here, so the address is entirely the kernel's and the members actually read are
at their own natural alignment. Suppressed with `no_sanitize("alignment")` on
that one function — not an environment variable and not a build-wide flag,
because 62 lines of known-benign noise is how a real finding gets scrolled
past.

To repeat the run:

    cmake -S diagnostics/jsrf_first_fault -B <dir> \
          -DRECOMP_GEN_DIR=<gen> \
          -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake --build <dir> -j 8 --target <every jsrf_*_test except
                                       jsrf_d3d11_copy_test (Windows-only) and
                                       jsrf_vsh_reference_test (needs a path)>
    ctest --test-dir <dir> -j 4 -V      # -V, NOT --output-on-failure

### NEGATIVE RESULTS, so they are not re-run

- **53 mingw `-Wformat=` warnings are all noise.** Every argument is `ULONG`,
  `DWORD`, `LONG`, `HRESULT` or `ACCESS_MASK` — all 32 bits on Windows, same
  varargs promotion as the `%u`/`%d` they are passed to. No 64-bit, pointer or
  `size_t` mismatch anywhere, which is the shape that would be a real bug.
  Worth casting the arguments (never the specifier) when someone is next in
  those six files; not worth a sweep on its own.
- **`-Wnonnull` x4 in `kernel_bridge.c` are false positives.** All four are
  `memset(XBOX_TO_NATIVE(va), 0, N)` immediately after `if (!va) return;`. The
  macro carries a literal `: NULL` branch, which is what GCC sees; the guard
  already excludes it.
- **`-Wcast-function-type` x3 are one site**, `apu_shim.h`'s
  `CreateThread((LPTHREAD_START_ROUTINE)func, ...)` — the standard QEMU shim
  idiom, reported once per including TU.
- `-Wmisleading-indentation` x4 and `-Wtype-limits` x1 are stylistic
  (`if (r < 0) r = 0; if (r > 255) r = 255;` on one line; `unsigned >= 0`
  guarded by a second condition that does the work).

---

## State

Suite 105/106. The one failure is `jsrf_input_hotplug`, which reads live
hardware. `jsrf_switch_audit` moved from failing to green; the control-flow
gate holds; `symbolize.py check` still reads 1333/1347 exact.

Confirmed on a third completed 60 s run after every change above: guard
5524/5524 matched with 0 unmatched and max_depth 2, `[FLIP-SNAP] 3892 frames
published, 0 DROPPED`, `snap=0.02 ms` for the third time, `[PRESENT]` steady at
convert 0.08 / upload 0.22 / draw-submit 0.04 ms.

Clean under ASan+UBSan (0 findings, 105 tests) and TSan (the two concurrency
tests). `x86_64-w64-mingw32-gcc` reports no errors on any file changed here.

New: `src/kernel/frame_pool.h`, `src/d3d/d3d8_combiner_bits.h`,
`diagnostics/jsrf_first_fault/adx_guard.{c,h}`, `adx_guard_test.c`,
`frame_pool_test.c`, `combiner_output_word_test.c`.

No regeneration was needed: `jsrf_manual_overrides.c` changed only the bodies
of two already-excluded functions, so the `--exclude-manual` skip set is
unchanged.

## What the runs here did NOT test

`contended=0` in both runs (locks 9932/9932 and 5646/5646 matched, zero
unmatched, max_depth 2, no steals). The intro scene never has two threads in
the ADX region at once, so the *title* runs have not exercised the interleave —
only the unit test has. A cutscene is where the freeze was measured and is where the
guard needs a player session.

The present-path numbers are from the intro. Re-measure on gameplay before
committing to a presentation rewrite.
