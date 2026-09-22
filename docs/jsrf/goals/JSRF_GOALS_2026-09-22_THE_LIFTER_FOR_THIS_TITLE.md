# JSRF goals — the lifter, for this title, 22 September 2026

Supersedes `JSRF_GOALS_2026-09-21_NIGHT5_THE_GPU_HAS_TEXTURE_HARDWARE_AND_WE_NEVER_USED_IT.md`
for ORDERING ONLY, and only on the translator track. G1–G27 carry forward,
nothing there is retracted, and G27 remains the head of the renderer track.
The player asked, on 22 Sep, whether the lifter can be improved for JSRF
given what is now known, and then said to set goals and work the list. This
file opens **G28–G31** and orders them.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

## Why speed is not on this list

Measured, tutorial scene, `EARLY_Z_REF0` ceiling arm, 16.36 ms frame:

| stage | ms/frame |
|---|---|
| vsh + submit + sync + snap + clear (renderer) | 12.9 |
| **rest** — the title's compiled code, the kernel, the pushbuffer parser | **3.5** |

Everything the translator emits lives inside the 3.5 ms line, and not all of
it. Two earlier results bound it further: `-O2` removed the thread-local
register lookup entirely and bought **zero frame time** (`b1f0531`, 13 Sep),
and the 13 Sep profile put the process at roughly 195,000 samples waiting
against 14,000 working, with the only large guest entry being the ADX
busy-wait. Register-context rewrites, dead-flag elision (1.0–1.5% of guest
loads) and bypassing the GPU-ownership map on stack accesses all have a
ceiling under 3.5 ms and a real value well under it. They are not on this
list. They become worth measuring after G27 lands and a profile puts guest
code at the top.

## G28 — every switch dispatch resolves, or is known dead

**Measured on gen `46bb115c` (baseline `control_flow_baseline.json`):** 142
unresolved switch dispatches across 53 tables. The failure path of the
fallback (`RECOMP_ITAIL` miss) pops a return address the `jmp` never pushed
and carries on with `eax = 0`.

**All 139 classifiable sites share one shape** (`scratchpad/switch_why.py`,
22 Sep): the dispatching body is a `tail_jump_alias` whose extent ends
**exactly at the first arm** of its own table — `sub_00020420` is
`0x20420–0x207C2` and the table at `0x208E4` reads `0x207C2, 0x20814,
0x208BA, 0x208DB`. The lifter's `≥ 2 arms inside` rule then cannot fire.
This is the state `_alias_end` (functions.py, 13 Sep) exists to repair, and
on this gen it does not: the engine knows every one of these tables and
their entries, the body walk reaches the dispatch, and the re-measure still
returns the old end. The defect is in the walk after the dispatch, and it is
being traced now.

**Liveness, which decides how much this is worth:**

| | count |
|---|---:|
| tables that resolve in some other body (`r ≥ 1`) | 32 |
| tables that resolve **nowhere** (`r = 0`) | 21 |
| alias bodies holding the 139 sites | 69 |
| of those ever observed as an indirect-call target (icall DB, 1,189 targets) | **0** |
| of those called or tail-jumped directly anywhere in the gen | **0** |

So no site is proven live, and none is proven dead either — the 21 `r = 0`
tables have arms translated nowhere, and six of them sit in `0xC0DE0–0xD5DD8`,
a region the decompilation has no names for. A scene we have not played can
reach them.

**Done when:**
1. The gate's `switch_unresolved` falls and `switch_resolved` rises by the
   same tables, with no other `MUST_NOT_RISE` measure moving. Accept with
   `--write-baseline` in a commit that names the tables.
2. Every table still unresolved after that is listed by address with the
   reason, in this file.
3. One scene-matched scripted run on the new gen, tutorial, 0 guest faults,
   `[ITAIL]` unresolved = 0, and draws-per-flip within variance of the
   pre-G28 gen. The regeneration is owed anyway and this rides on it.

### G28 outcome, 22 Sep 2026 (night)

**The cause was not table detection.** Four detector runs at 20 s each
(`scratchpad/alias_diag*.py`; see the memory note on running the detector by
hand) found it in three layers, each hiding the next:

1. **Alias extents were stale.** An alias is recorded as running to the end
   of the body it lands in, and that body was measured while a switch arm
   was still a seeded start, so it ended at the arm. The seed-interior pass
   then dropped the arm and re-measured the real bodies, but the alias
   record kept the old end, and `_alias_end` only ever grew an alias. Each
   alias's own flow ended at its `ret` long before the dispatch, so 139 of
   the 142 unresolved sites were `jmp [reg*4 + table]` translated as **dead
   code** inside over-long alias bodies. Fix: an alias whose recorded
   boundary no longer exists as a start ends where its flow ends
   (`functions.py:_alias_boundary_gone`).
2. **The dead padding hid the real owners.** `0x000D1440` and `0x000D4470`
   own 21 of the tables between them, are reached only as immediates
   (`data_imm` from `0x000D3A28`, `0x000D5004`), have no prologue, and open
   with the dispatch. The imm-ref pass tested coverage, the padding covered
   them; once it did not, the returning-body probe still refused them
   because it ended at any `jmp` that was not an immediate. Fix: the probe
   follows a dispatch through a table the engine has measured into its
   arms (`engine.py:probes_as_returning_body`), and `detect()` re-runs the
   two gap passes once aliases have real extents.
3. **Two things the re-run exposed, both pre-existing.** The imm-ref pass
   tested coverage with a nearest-start lookup that is wrong once bodies
   overlap (it carved five starts out of `sub_0003FEC0`); and the
   gap-prologue pass accepted `mov edi, edi` padding two bytes before an
   inline jump table as a function start -- **70 such "functions" existed
   in the shipped gen**, decoding tables as code, and they carried 69 of the
   123 untranslated-instruction sites. Both fixed.

**And one lifter limit.** Eight tables live in `.rdata` (`0x001FA008`,
`0x0020D9DC`, `0x002174BC`, `0x00216148`, `0x0021FF88`–`B0`); the engine
refused them on a same-section rule, and the lifter, reading a table blind,
stops at the first entry outside the function, so a switch whose cases
alternate between local arms and other functions collapsed to nothing. Now
the disassembler exports every measured table (`disasm/jump_tables.json`),
`tools.recomp` reads it from `--disasm-dir`, and an arm that is another
function becomes the tail jump a direct `jmp sub_X` becomes: **40 such arms**
in the new gen.

**Gate, gen `52b6b3f8` against baseline `46bb115c`:**

| measure | before | after |
|---|---:|---:|
| switch_unresolved / tables | 142 / 53 | **1 / 1** |
| switch_arms_tight / no_body / missing_arm | 522 / 6 / 1 | 19 / 0 / 0 |
| untranslated sites (`todo`) | 123 | 39 |
| unresolved stubs | 168 | 110 |
| switch_resolved | 533 | 504 (duplicate dispatches in dead tails; the gate verified no table lost a placeable site) |
| functions | 8,839 | 8,744 |
| coverage bytes | 1,615,561 | 1,613,800 |

Coverage fell by 1,761 bytes and every lost byte is accounted for: 6,000
lost, of which 3,994 lay inside the 70 removed table-decoding "functions"
and 2,006 inside shrunk alias tails unreachable from their entry; 0
elsewhere. 4,239 bytes gained. Baseline accepted with `--write-baseline`.

**The one table left, with its reason.** `0x00076610`, dispatched from
`0x00075EAC` inside `sub_00075E90`: the detector finds no function there --
nothing calls, jumps to, or takes the address of `0x00075E90` in any form
the passes read -- and the translator's own "recovered entry" for it ends
at `0x000761EA`, short of arms that reach `0x000765FA`. The three dispatch
sites at `0x0017D0CE`/`0xF8`/`0x12A`, `0x000331F9` and `0x00076679` are the
same class, coverage holes, and were unresolved before too. They need an
entry-point source the detector does not have; the decompilation names none
of them.

**Reverted along the way:** a patch letting a dropped seed keep a tail-jump
alias, written for the 41 aliases that vanished between trees -- which turned
out to be interior labels of functions that now cover them, so the patch
answered nothing this title shows.

**Exit criterion 3, met.** `measure.sh g28 150` on the new gen, audio device
off, reached the Corn tutorial (`live=61`), **0 guest faults, `[ITAIL]` 0,
`[ICALL]` failures 0, `[UNIMPL]` 0**. Draws per flip 68.3 (536,338 / 7,852)
against the reference arm `ezref` on the old gen at 67.8 (697,226 / 10,287).
Cumulative frame mean including boot 17.83 ms against 16.53 ms over a longer
run; that is not the scene-matched instrument and is recorded only so nobody
reads it as one. The player's bundle was rebuilt from this binary.

**G28 is done.** What it leaves for later: the five coverage holes above,
and the icall-fallback path of a resolved switch still observing its arm as
an "indirect target" for the feedback database (the classifier drops those
as `switch_arm`, so it is contained, not fixed).

Pre-G28 trees are preserved: `~/jsrf-build/jsrf-first-fault/gen.pre-G28-20260922-KEEP`
and `disasm.pre-G28-20260922-KEEP` (with `vtable_seeds_accum.json`).

## G29 — indirect calls that name their target

JSRF is vtable-heavy: 6,256 vtable methods, 7,395 `RECOMP_ICALL` sites,
846 distinct targets observed in one session. Every site today makes an
out-of-line `recomp_lookup_manual` call, a flat-table lookup, and three
writes to `g_icall_trace` / `g_icall_trace_idx` / `g_icall_count` — plain
globals shared with the four ADX threads, so every indirect call on any
thread contends the same cache line.

The feedback database records **targets, not sites**, so the lifter cannot
speculate. This is the half of Microsoft's `VirtualDispatchTraceFiles`
mechanism that `icall_feedback.py` did not copy.

**What to build, in order:**
- a. Runtime: record `(site, target)` pairs under `RECOMP_ICALL_FEEDBACK`,
  dump them beside the existing target list. The site is the guest PC the
  lifter already passes to `RECOMP_MEM_WRITE32`; the macro needs the same.
- b. Feedback tool: persist per-site target sets, cumulative, in the same
  JSON; the target-only list stays as it is so `--seed-functions` is
  unaffected.
- c. Lifter: for a site with a recorded set of ≤ 4 targets, emit
  `if (_va == T) sub_T(); else RECOMP_ICALL_SAFE(...)` — the generic path is
  the fallback, so a new target is never wrong, only slow. The gate gains an
  `icall_guarded` measure.

**Done when:** the gate counts guarded sites; a run reports guard hits and
misses; a scene-matched A/B shows frame time neutral or better; the ABI
checker (`RECOMP_ABI_CHECK`) reports no new violation. Value is unmeasured
and the frame is not here (see above), so this is third, not first.

## G30 — the lifter fuzz, re-scored on the current lifter

The 17 Sep run (`PROGRESS_2026-09-17_NIGHT_THE_LIFTER_FUZZ.md`) scored **860
mismatches across 60 cases: bit=34, shift=20, cmov=3, incdec=2, cmpset=1**.
Fixes landed on 18–19 Sep (SF from a compare, flag settle at a loop head,
three-push SEH prolog). Nobody has re-scored. `unicorn` is not installed for
`/usr/bin/python3`; the fuzz doc's recipe is a venv with `unicorn` and
`capstone`.

The visible symptom this could own: the DirectSound fault at
`sub_001A2E2E +0x670` (`0xFFFFFFB4`/`0xFFFFFFBE`), which ended the 2040 s
player session and was one run in eight on 15 Sep. It is XDK code the fuzz
can reach and nothing else has explained it.

**Done when:** the same seed and count run against HEAD's lifter, the
per-family mismatch table is in this file, and each surviving family is
either fixed with a test under `tools/recomp/` or recorded with a count of
sites in the JSRF gen that emit that form.

### G30 outcome, 22 Sep 2026 (night)

Environment: `~/jsrf-build/fuzz-venv` (unicorn 2.1.4, capstone 5.0.7). The
fuzzer itself needed two repairs before it would run at all: since G22b the
lifter emits `RECOMP_UNIMPL(...)` instead of a bare comment for an
instruction it cannot translate, which the harness's stub runtime did not
define (every batch failed to link) and which its "unlifted, skip" test did
not recognise. Both fixed in `tools/conformance/fuzz_unicorn.py`.

| run | cases ran | vectors | mismatches |
|---|---:|---:|---:|
| 17 Sep, seed 7, count 600 | 542 | 13,008 | **860 across 60 cases** (bit 34, shift 20, cmov 3, incdec 2, cmpset 1) |
| 22 Sep, seed 7, count 600, HEAD lifter | 542 | 13,008 | **24 across 1 case** (incdec 1) |
| 22 Sep, seed 7, generator fixed | 545 | 13,080 | **0** |
| 22 Sep, seed 0x11, count 600 | 539 | 12,936 | **0** |

The one surviving case was `not ax ; setnz dl`. NOT leaves EFLAGS alone
(the lifter already lists it in `_EFLAGS_PRESERVE`), so the `setnz` read
flags from before the case: undefined in both models, and the fuzzer's
inc/dec family was emitting it as if it were a producer. Generator fixed;
the lifter was right. The 18–19 Sep fixes (SF from a compare, bts/btr/btc,
narrow rotates, flag settle at a loop head) account for the rest of the
860, and this is the first time that has been shown rather than assumed.

**The DirectSound fault is not the lifter's.** The player's 2040 s session
log records it exactly: `sub_001A2E2E +0x6A4`, `EAX=FFFFFFB4`, read at
`EAX+0xA`. The guest code is `mov eax, [esi+0x50]` (a voice's prev link),
`cmp ecx, eax` against the list head, and on "not the head":
`movzx edi, byte [eax+0x18]; add eax, -0x4c; movzx eax, word [eax+edi*2+0xa]`.
The link was **0**: neither the head it is compared against nor a node, and
the lifted C is exactly that x86. A NULL prev-link in the voice list is a
guest-state defect (ours or the APU model's), and no fuzz can reach it.
G30's claim on that symptom is withdrawn; the fault keeps its own line in
ACCURACY_GAPS.

## G31 — the decompilation as a verifier for what we do not translate

The gate's 123 `todo` sites are all `bound`, `arpl`, `hlt`, `sti`, `daa`…
read from zero padding in DSOUND and DOLBY. The last 190 s tutorial run hit
none (`[UNIMPL]` = 0). The decompilation's `symboltable.tsv` carries 405 data
rows and a calling convention per function. **Licence: verify against it,
never generate from it** — no hint file derived from it enters the repo.

**Done when:** a checker outside the repo (beside `jsrf-symbols-merged.tsv`)
confirms every `todo` site lies in a region the decompilation marks as data
or padding, and one scripted run with `RECOMP_UNIMPL_TRAP=1` completes. Any
site that fails either test is a real untranslated instruction and gets its
own line here.

### G31 outcome, 22 Sep 2026 (night)

After G28 the gen carries **38** untranslated sites (from 123). The
decompilation cannot judge any of them: its 405 data rows lie thousands of
bytes from every site and it names no function containing one -- the
checker (`~/jsrf-build/decomp-check/check_todo_sites.py`) reports 0 of 38
inside a decomp data row. So the verdict comes from our own detector:

| class | sites |
|---|---:|
| inside a function's extent but **unreachable from its entry** by any flow (dead bytes: a five-entry `enter` table at `0x2CC01`, a `daa` byte run at `0x1028F9`, …) | 29 |
| at the head of a **fake function carved from data**: `sub_000465A0` (a 16-byte vtable-thunk seed over `3f 65 04 00 61`) and three `cc_boundary` starts inside XDK data (`0x1A7788`, `0x1BC778`, `0x1C3F18`) | 9 |
| a real instruction the lifter cannot translate | **0** |

Two 150 s tutorial runs, one with `RECOMP_UNIMPL_TRAP=1`, reached `live=61`
with **0 `[UNIMPL]` hits and 0 faults**. The trap arms lazily on the first
hit, so a clean run shows no banner; the instrument is the hit counter on
the ICALL cadence, present at all 38 sites. G31 is done. The four fake
functions are a detector item for later: a seed that decodes to `aas` and a
cc-boundary start inside a data section should both be refused.

## The order

1. **G28** — the only item with a known-wrong branch in shipped code.
2. **G30** — an afternoon, local, and the one item with a player-visible
   symptom to claim.
3. **G29** — largest, unmeasured value, needs a run to feed it.
4. **G31** — cleanliness unless the trap run says otherwise.

Exit for the file as a whole: the regeneration that G28 needs carries every
pending translator change (G28, any G30 fix), the gate is re-baselined once
with the reasons written down, and one scene-matched run confirms the title
still plays.

## R — refuted or retired on this track

| idea | why not |
|---|---|
| register-context pointer instead of TLS | measured negative 12 Sep at -O0; `-O2` then removed the cost anyway |
| dead-flag elision | 12–16% of flag sites, 1.0–1.5% of guest loads; clang already deletes the stores |
| `_recover_cfg` is the gap (upstream note, 21 Sep) | its narrow operand test only matters for functions being re-measured; every unresolved site here is an alias whose extent stops at its own first arm, and the engine already knows the table |
| function detection is incomplete | 14 of the decompilation's 1,347 function starts are missing from ours, 12 in the XDK region |
