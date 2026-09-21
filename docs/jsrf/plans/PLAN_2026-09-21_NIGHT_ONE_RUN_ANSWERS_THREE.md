# PLAN -- 21 September 2026 (night)
# STOP RE-DERIVING WHAT IS ALREADY WRITTEN DOWN

Successor to `PLAN_2026-09-21_WHAT_THE_SYMBOLS_MAKE_POSSIBLE.md`, written
against `HANDOVER_2026-09-21_COUNT_THE_INPUT_NOT_THE_GATE.txt` at HEAD
`8d765ed`, tree clean.

The handover's method carries forward: **a rejection path that names the gate
and not the value is a blind spot; count the input.** This plan adds a second
one, which is the reason the order changed:

> **A fact we re-derive every session is a fact we are not using.** Four
> CActMan offsets have each cost a session of hand-disassembly and each one
> was already written down, in a header we have had checked out for two days.

---

## 0. THE TYPE LAYER -- NEW, AND IT GOES FIRST

We hold two databases and use both as a **log pretty-printer**.
`merge_symbols.py` flattens each to `address -> name` for `symbolize.py`, and
that is the whole of it. Three richer layers are being dropped. All three
were confirmed in the tree, not assumed.

### 0a. The struct layouts are ground truth, and this is now measured

`JSRF-Decompilation/decompile/src/JSRF/Action.hpp` carries the full `CActMan`
with named fields. Compiled at `-m32` and `static_assert`ed against every
CActMan offset this corpus derived independently, by disassembly, across
three separate sessions:

| field | the header | our side | independent? |
|---|---|---|---|
| `m_bFatal` | +0x24 | `main.c:302`, commit `5402ebc` **2 Sep** | YES |
| `m_bSkipDraw` / `m_DrawMode` | +0x74 / +0x94 | `startup_probe.c:703`, commit `770cf92` **4 Sep** | YES |
| the four mode flags | +0x40..+0x4C | `startup_probe.c:703`, same commit **4 Sep** | YES |
| the eight `*NextFrame` triggers | +0x50..+0x6C | `startup_probe.c:703`, same commit **4 Sep** | YES |
| `m_lpDrawRoot` / `SortRoot` / `SortBinRoots` | +0x7FA4 / +0x7FAC / +0x7FB4 | `main.c:2402` | **NO -- see below** |

**CORRECTED 21 Sep, and the correction matters.** An earlier draft of this
table listed the draw-list row as independent corroboration. It is not.
`git log -S 0x7FA4` puts its first appearance in commit `f2f6e57`,
**10 Sep, "Borrow the decompilation's names"**, and `main.c:2398` says so in
its own words: *"Per-object offsets are the xemu tool's, which came from
CActBase in the decompilation."* Asserting that row against their header is
circular -- it checks transcription fidelity and drift, which is worth having,
but it is not evidence. Most of `CActBase` is in the same position.

The checker therefore tags every assertion `[IND]` or `[TRA]` with a
`path:line`, and reports the split: **61 assertions, 40 independent, 21
transcribed.** Nobody should ever cite the total as if it were all
corroboration.

**Every assertion holds. No contradiction anywhere.** Those offsets fall out
of `m_lpActTbl[eACTID_CNT=7668]` and `m_Globals[eGLOBAL_CNT=461]` being
correctly sized -- a layout that cannot agree by luck at +0x7FB4.

**THE LICENCE DECIDES THE DESIGN, AND IT RULES OUT THE OBVIOUS BUILD.** The
decompilation publishes no licence file and its readme disallows LLM use in
the repository. So: **read it in place, verify against it, never vendor it.**

  - What lives IN our repo: `jsrf_guest_types.h`, offsets and accessors we
    assert ourselves, replacing the prose comments in `main.c`.
  - What lives OUTSIDE it: a checker, beside `jsrf-symbols-merged.tsv` in
    `~/jsrf-build/`, that compiles their header and `static_assert`s OUR
    numbers against it. Divergence becomes a build failure.

That gets the whole benefit -- a wrong offset is a compile error instead of a
wrong number in a log -- while copying nothing. It is exactly the check run
by hand today, made repeatable.

### 0b. 405 rows are discarded by an off-by-one, and G22 wants them

`merge_symbols.py:34` keeps decomp rows with `len(f) > 6`. The 405 `data`
rows have five fields, so every one is dropped in silence. Among them:

    0x0007be08  data  void *[2]   CActSequence::Exec0Default__jumptargets
    0x0007be10  data  byte[28]    CActSequence::Exec0Default__jumptable

Named jump tables, with exact element type and count. **G22 in the live goals
file is "control-flow completeness: every edge accounted for"**, instrumented
by `[ITAIL]` / `[ICALL]` unresolved-target counting. This is ground truth for
indirect-branch resolution and a field-count test is throwing it away.

Fix the filter; route the jump-table rows into G22's edge work rather than
into `symbolize.py`, which has no use for them.

### 0c. The XDK device fields the audit already asked for

`XbSymbolDatabase/src/OOVPADatabase/D3D8/4134.inl` is JSRF's exact XDK. The
D3D8 database carries named `D3DDevice` **field offsets** --
`m_RenderTarget`, `m_DepthStencil`, `m_Textures`, `m_PixelShader`,
`m_Palettes` -- of which only four reached `jsrf-symbols-merged.tsv`, and
those four are mislabelled `func` in it because the merge has one row shape.

`PLAN_..._WHAT_THE_SYMBOLS_MAKE_POSSIBLE.md` §0 names the D3D8 device struct
as the first target of the value audit and says its fields have had "exactly
as much scrutiny as the fence had -- which is to say, none". Two of them,
`m_RenderTarget` and `m_DepthStencil`, are also the two most relevant to
black geometry (item 5) and to the boost-dash flicker (item 8).

### 0d. What the decompilation does NOT give us. Do not plan around it.

The `.cpp` bodies are mostly stubs -- `Action.cpp` defines 28 and leaves 67
empty, and while most of those empties are genuinely-empty base virtuals,
`CActMan::setEventNextFrame` is a stub too. Only `ActSequence.cpp` (52 real)
and `SaveData.cpp` (60 real) carry substance, and only five translation units
exist at all.

**There is nothing for the graffiti shader, the tag manager or the text
renderer.** Items 6 and 7 still need disassembly. The value here is headers,
enums and the symbol table -- not readable logic.

---

## 1. A CORRECTION THE TYPE LAYER FORCES, BEFORE ANY CODE IS WRITTEN

Handover 2d states the four mode flags "are LATCHED -- entered and never
exited is permanent". There are eight more fields immediately after them, at
+0x50..+0x6C:

    BOOL m_bCoveredPauseNextFrame;   BOOL m_bNoCoveredPauseNextFrame;
    BOOL m_bEventNextFrame;          BOOL m_bNoEventNextFrame;
    BOOL m_bFreezeCamNextFrame;      BOOL m_bNoFreezeCamNextFrame;
    BOOL m_bUncoveredPauseNextFrame; BOOL m_bNoUncoveredPauseNextFrame;

together with `CActMan::setEventNextFrame(BOOL)` and its three siblings, and
the header's own note on precedence: `coveredPause > Event > FreezeCam >
UncoveredPause > Default`.

**AND WE FOUND THIS ALREADY, BY DISASSEMBLY, SIX DAYS BEFORE THE HEADER WAS
OPENED.** `startup_probe.c:698`, commit `770cf92`, **4 Sep**:

    /* +0x50..+0x60 are the triggers sub_00013A80 tests each tick and
     * +0x40/+0x44 the flags it derives from them. */

So the header did not reveal the deferred-clear channel. It **named** a thing
this project had been sampling for seventeen days and never identified -- and
that is the more accurate statement of what the type layer buys. It also hands
step 3 something better than a field list: **`sub_00013A80` is the per-tick
consumer**, and "asked and dropped" is only visible there.

So Event mode has an explicit **deferred-clear channel**. "Latched" is the
symptom, not the design, and the probe the handover specifies cannot see the
difference between:

  - nobody ever asked to leave Event -- `m_bNoEventNextFrame` stays 0; the
    fault is upstream, in whatever should have requested the exit; or
  - somebody asked and the request was dropped -- `m_bNoEventNextFrame` sets
    and is never consumed; the fault is in the frame-boundary handler.

**Different bugs, different fixes, one extra `printf` argument.** Read
+0x40..+0x6C -- **twelve** dwords (4 mode flags + 8 requests), not four. An
earlier draft said thirteen; thirteen reads one past the block into
`m_bDrawChildren`, which toggles and makes the reporter repeat itself forever.

---

## 2. THE ORDER

### 1. THE FREE RUN. No build, and it must come before any source edit.

    SDL_AUDIODRIVER=no_such_driver \
      play_scripted.sh intro-census @diagnostics/jsrf_first_fault/pad/intro.pad 120
    SDL_AUDIODRIVER=no_such_driver \
      play_scripted.sh still-census @diagnostics/jsrf_first_fault/pad/still.pad 180

First because it needs nothing built and **must not overlap a rebuild** --
`play_scripted.sh` exits 1 on a stale binary and kills the rest of a loop. Arm
`RECOMP_FB_DUMP` into a fresh directory and preserve it; every run overwrites
the images.

Two facts make this cheaper than the handover assumed. `pad/intro.pad` already
exists and presses nothing -- handover 2b asks for a boot-and-wait scenario
that is already written, and its header says why: "not one capture taken today
contains the intro animation, because every one of them skipped it."
`pad/still.pad` is the other half, reaching the Corn tutorial then stopping at
t=47 by design, so the pair covers both readings of "DJ intro cutscene". And
`nv2a_texture_copy_census()` (`src/nv2a/nv2a_texture_copy.c:229`) is always
compiled in and silent when clean, so **no switch and no rebuild**.

Three questions off the two logs:

  a. Does the black geometry reproduce unattended? The tutorial refuses
     nothing and renders correctly; if the intro also refuses nothing and is
     still black, handover 2b is confirmed from our own capture.
  b. Does the combiner census speak? The player's session shows 59 draws
     refused "combiner output mode"; the tutorial shows none. A
     `[COMBINER] refused output word 0x000820D0` line means item 7 has a
     reproducer that needs no player.
  c. Is there a fourth refused format? `[TEXFMT]` is silent when clean, so a
     table appearing at all is news.

### 2. THE TYPE LAYER. §0 above. No run, no guest, pure build-time.

0a, 0b and 0c in that order. 0a is the one that pays immediately, because
step 3 depends on it. None of this touches the guest, so it can be reviewed
and rebuilt without a scenario in flight.

Pass condition for 0a: the out-of-repo checker compiles and every assertion
holds, including the four in the table above -- reproducing today's result
mechanically. Pass condition for 0b: the dropped-row count goes to zero and
the jump-table rows are counted, not merely parsed.

### 3. THE ACTMAN REPORTER -- NOW TWELVE FIELDS. **DONE, ARMED, SILENT.**

Handover 2d's "cheapest test", and still cheap: `main.c:295` already reads
`MEM32(0x22FCE0u)` and line 1271 already has a periodic report site beside
`jsrf_scene_report()`. It prints **named** fields, +0x40..+0x6C, and §1 above is why the extra eight
matter.

**RESULT, 21 Sep, `actman-probe` over `still.pad`, 124 s of gameplay:** every
one of the twelve reads zero and `effective=Default` throughout. The
instrument works and the defect did not appear.

**DO NOT READ THAT AS "the banner is fine".** The sampler runs inside the
periodic report, so a mode entered and left between two samples leaves no
trace -- and `still.pad` parks the player after Corn's dialogue, so no
objective ever advances, which is the very thing the banner defect needs.
All-zero here means "not latched at any sample", nothing more. The next step
if no latch is caught this way is to move the sampler onto `sub_00013A80`'s
own tick, where every transition is visible.

Still the only instrument here that can settle two symptoms with one number:
if Event mode is held and never released, the banner shows the previous
objective's string *and* an ADX voice line never reported completion. One root
cause under two items.

`diagnostics/` is not generated code, so this is a cheap build.

**Watch `sub_00013A80` as well as the fields.** It is the per-tick handler
that tests the +0x50..+0x60 triggers and derives +0x40/+0x44 from them
(`startup_probe.c:698`). A request that is set and then dropped is visible
only at the consumer; thirteen field values sampled between ticks cannot
distinguish "never asked" from "asked and lost".

Pass condition: a run through Corn's dialogue shows the mode flags returning
to zero. A flag that enters and stays is the finding -- and its `No...NextFrame`
sibling says which of the two bugs it is.

### 4. FORMAT 0x04. Same build as step 3.

`0x04 A4R4G4B4` swizzled, 11 textures, e.g. `Media/Stage/Stg52_t.dat`. The
shipped-asset enumeration gives `{0x0C, 0x0E, 0x03, 0x06, 0x07, 0x04}` as the
complete set and this is the last member refused; it closes the format
question for this title.

Test it in the shape the 0x07 case established -- failing with a **value**,
not with "a word was written". The 0x07 alpha test reads "got 0 want 255" when
the one alpha line is reverted; the 0x04 test should fail the same way on the
4-bit expansion, because `A4>>4` and `A4*17` differ by exactly the kind of
amount nobody notices in a screenshot.

**Bundle steps 3 and 4 into ONE build. Not two.**

### 5. THE BLACK GEOMETRY, once step 1 says where it reproduces.

Nothing is refused, so it is a shading error in a path we accept -- the
obvious theory is dead and the handover killed it. With 0c done, the first
values to audit are `m_RenderTarget` and `m_DepthStencil`, on the same
principle that caught the fence: **check the units and range the consumer
assumes, not the logic of the gate.**

### 6. THE THIN LINES. A python tool, no runtime change to start.

Reproduces in our own 90-second tutorial capture (`combiner/picture-11.bmp`,
Corn's "This is th"): short horizontal marks at the TOP of the glyph cell.
`clobber.py` scored 0/663 on the runs that show it, and that is not a
contradiction -- it compares quad *positions and cells*, and this is *pixels*.

The instrument is the missing thing, not the theory. Crop the text region from
the preserved captures and difference the glyph cells against themselves; cell
bleed shows as a consistent offset, not as noise. Only then choose between the
two candidate mechanisms -- the shared edge of the quad's two triangles, or a
V coordinate reaching into the next cell.

Do NOT arm a guest-store watch for this. Three runs with `RECOMP_MEM_WATCH`
live all scored below the unperturbed baseline: the watch perturbs what it
measures.

### 7. THE GRAFFITI COMBINER. The biggest win, the biggest job, and unaided.

Gated on step 1b. Do not open it until the census says which refusal fires and
how often -- that is the whole lesson of the morning. Per 0d, the
decompilation does not cover this; it is disassembly.

The work, in the handover's priority order:

  1. output words above `0xfff` -- `AB_DOT_PRODUCT` (bit 13) and
     `AB_BLUE_TO_ALPHA` (bit 19). Gate at `nv2a_texture_copy.c:68`.
  2. outputs routed to T0-T3 (dst 8-11). Gate at `:73`, which models only
     discard/R0/R1. Stages 1, 3 and 4 of the graffiti shader do this.
     `regs[14][4]` already has slots 8-11, so no array change is needed --
     the gate is the only thing in front of them.
  3. nonzero per-stage C0 -- `M(0xa60+4*j)` / `M(0xa80+4*j)`. Gate at `:91`.
     Four OTHER shaders share this case, so the value is not graffiti-only.

**Times THREE consumers, not two.** The handover's 2a says "one model,
expressed twice". There is a third:

    src/nv2a/nv2a_texture_copy.c:605   combiner_output()   -- CPU reference
    src/nv2a/nv2a_metal.m:680          the Metal shader string
    src/nv2a/nv2a_d3d11.c:148          k->cocw[i] / k->aocw[i]  -- Windows

The Windows build runs. Widening the gate without widening `nv2a_d3d11.c`
gives Windows accepted draws it cannot shade -- the same defect class as
accepting 0x07 without sampling its alpha. Write the CPU reference first and
make it the oracle: it is the one of the three that unit-tests without a GPU.
A widened gate that reaches a backend ahead of its decoder renders *wrong*
instead of *nothing*, which is strictly harder to see.

### 8. BOOST DASH. One question, asked before any work.

Reported 21 Sep, uninvestigated. **Does it predate `5358eec`?** Boost dash is
the kind of effect that uses a render target the guest now waits on, so it is
the first symptom that could plausibly have been caused by the fence fix. Ask
the player, or run the parent commit's binary, before assuming it is old.

### 9. RE-ESTABLISH THE AUDIO CONTROL. No build, but it needs the clock.

The fence fix changed when the guest yields, so every timing measurement taken
under the old wait-free regime is void -- control (34 dropouts, first at
150.2 s), `RECOMP_WILD_PTR=0`, `RECOMP_METAL_NO_DEPTH_SYNC=0` and the flag on
it. The three untested arms must not be scored against the old control.

Re-run control first, with the >=214.8 s window the corpus requires. It needs
a live audio device, which is the arm where the scripted boot is one-in-three,
so it is scheduled work rather than a quick check. **Deliberately last:** if
step 3 shows Event mode held by a stream that never reports completion, the
audio defect has a cause and this bisect is measuring a symptom.

---

## 3. BACKGROUND, WHEN THERE IS NO RUN IN FLIGHT

  - **The device-struct audit**, now with 0c's names. Every value we put where
    the guest reads it, checked against the arithmetic that consumes it.
    `WRITE_CURSOR 0x00`, `LIMIT 0x04`, `RING_LO 0x24`, `RING_HI 0x28`,
    `PUT 0x30` have had as much scrutiny as the fence had, which is none. Each
    invariant found becomes a test the same day: the fence's was
    `*fence <= [dev+0x30] - 2`, and one assertion of it would have failed on
    day one.
  - **Every `return "reason"` carries its value.** Two censuses exist; the rest
    of `src/nv2a` and `src/kernel` do not. Finite, mechanical, and it is what
    turned a four-week-old log line into a one-run answer.
  - **Symbols always-on.** Wire `RECOMP_XDK_SYMBOLS` into the harness default
    and make `merge_symbols.py` a build step. The 20 Sep runs show the
    fallback path is what actually resolves `D3D8__D3D_g_pDevice`.

## 4. TRAPS THAT APPLY TO THIS PLAN SPECIFICALLY

  - **Never vendor the decompilation.** No licence file, and an explicit LLM
    policy in its readme. Read in place, verify against, copy nothing, push
    nothing back. 0a is designed around this and the design is not optional.
  - **The decompilation names, it does not explain.** Its own source is under
    1% matched. A name is a lead, not a mechanism.
  - **`[PB-ACK] already=` is DEAD, not merely small, and the pusher-rate
    figure has now been wrong twice.** Both measured 21 Sep on the
    post-`5358eec` binary, 124 s of gameplay, 89 samples:

    | claim | where | truth on this binary |
    |---|---|---|
    | "GET moves about four times a second" | `main.c`, original | median **64,631 loops/s** |
    | "stale by ~30x; 6,810-6,932 loops/s" | 20 Sep correction | also stale, **~9x low** |
    | "`already` reads ~0" | 21 Sep handover | **frozen at 27,838, never incremented once** |

    The 20 Sep figure was taken before the guest ever blocked; making it wait
    where the hardware says it should gave this thread roughly nine times the
    CPU. `already` counts `MEM32(getp) == fence_counter`, which `5358eec` made
    impossible -- its 27,838 accumulated during boot before the release path
    went live, and it has not moved since. Read `acked` (99.21% of 10,286,519)
    and `not-consumed`. If `already` ever climbs again, the fence regressed.

    **The general rule this keeps paying for:** a rate written into a comment
    is true of one binary. Date it or do not write it.
  - `clobber.py` scoring 0 means "the clobber is fixed", never "the text is
    fixed". Step 6 exists because it is blind here.
  - Preserve `RECOMP_FB_DUMP` output before the next run; every run overwrites
    it.
  - Read completed runs only. A mid-run counter has misled three times; check
    `pgrep` before quoting a number.
  - No source edits while a run is in flight. This is why step 1 precedes
    step 2 even though step 2 is the headline.
