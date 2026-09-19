# JSRF goals — one root cause under two items, 19 September 2026

Supersedes `JSRF_GOALS_2026-09-18_NIGHT5_THE_2D_BIN_STOPS_ALONE.md` and the
"what to do next" list in
`HANDOVER_2026-09-19_DAY_THE_STUTTER_IS_THE_FRAME_RATE_AND_THE_FREEZE_IS_THE_GATE.txt`.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

G1–G19 carry forward. The switch-arm defect takes **G20**.

## What changed

Handover items 0.1 (the ADX freeze) and 0.3 (the skating crash) are **one
defect**, and it is neither an audio bug nor a scene-graph bug. Full evidence in
`docs/jsrf/progress/PROGRESS_2026-09-19_THE_FREEZE_AND_THE_CRASH_ARE_ONE_SWITCH_ARM.md`.

| | measured | source |
|---|---|---|
| the freeze | an unresolved indirect call spinning on NULL, to 2.3e10 calls | corpus, 17 runs |
| its root cause | a switch arm the lifter could not place → `RECOMP_ITAIL` | disasm + gen |
| the damage | `g_esp += 4` on a frame still holding locals → **0x60 stack deficit** | `recomp_types.h:1380` |
| it closes to the byte | predicted esp vs observed, 4 of 4 player crashes | crash logs |
| scope | **607 of 711 arms unreachable, 76 of 84 tables** | `switch_arm_audit.py` |
| a *second*, unrelated freeze | `NV2A_PMC_UPMIRROR` removes it: 0/87 vs 70/200 | date-controlled |

## THE ORDERING CONSTRAINT — read this before planning any run

A recording is **refused** when its `#!gen` does not match the running binary
(`src/input/xinput_device.c:1427`). The G20 fix changes
`function_bounds.json`, which forces a regeneration, which changes the gen id.

**Therefore every recording taken before the regeneration is worthless after
it.** Land G20 first, build ONE binary carrying both the fix and the recorder,
and only then spend a player session. Recording first wastes the scarcest
resource in this project.

## The order

### 1. G20 — three live targets, three different fixes

**Only THREE of the nine unresolved VAs are live on the current gen.** Checked
each directly: `0x0007E575`, `0x0006D7A8`, `0x000960C3`, `0x000FDB9F` and
`0x000F8B54` all already resolve to `goto` in the shared gen. The corpus tally
mixed runs taken against different gens. The live set:

| VA | what is wrong | fix | state |
|---|---|---|---|
| `0x00114B66` | owner truncated by a **spurious start** at 0x00114D34 | detector: short-jump-table map + `_arm_of_a_carved_run` | done, `wt-icall` |
| `0x000A5B8C` | **the containing function does not exist** | seed `0x000A5B60`, the real prologue | in flight |
| `0x001063CF` | extent clamped by a **real neighbour** 2 bytes below an arm | open | open |

**`function_bounds.json` CANNOT fix any of these, and an earlier draft of this
file said it could.** `functions.py:1540` reads

```python
for lo, hi in self._forced_bounds:
    if lo <= start < hi and hi < upper:
        upper = hi
```

It only ever *lowers* `upper`. A declared bound shrinks a function and can
never extend one, and nothing in it creates a missing start. It exists to stop
a function running past a translation unit — the opposite problem.

`0x000A5B8C`'s fix is a **seed, and specifically a seed of the prologue**:

```
000A5B5E  ret
000A5B5F  nop                <- alignment padding
000A5B60  sub  esp, 0x50     <- prologue
000A5B63  push ebx / ebp / esi / edi
000A5B67  mov  esi, ecx      <- __thiscall
```

`0x50 + 4 pushes = 0x60`, which is exactly the stack deficit derived
independently from the four crash ESPs. **Seed `0x000A5B60`, never
`0x000A5B8C`** — the prologue is a genuine function start and is what seeding
is *for*; the arm is an interior address and seeding it would truncate its
container. The two cases look identical in a log and are opposites.

**Done when:** `sub_000A5B60` exists with an epilogue, its switch resolves to
gotos, and the audit's 607 has fallen. **Needs no run.**

#### G20 outcome: two fixed, 76 parked, both routes measured dead

| VA | state |
|---|---|
| `0x00114B66` | **FIXED** — detector: short-jump-table map + `_arm_of_a_carved_run` (`wt-icall`) |
| `0x000A5B8C` | **FIXED** — seed the prologue `0x000A5B60` (`wt-walker`). A 44-byte "Recovered entry" stub ending in a bare `RECOMP_ITAIL` with **no epilogue** became a 1348-byte function with `POP32 edi/esi/ebp/ebx; esp+0x50` and the five-arm switch lifted to gotos. Audit 607 → 603, coverage **+93 bytes** = `0xA5BE9 − 0xA5B8C` exactly, starts net unchanged at 8866 |
| `0x001063CF` | **open**, and parked deliberately |

**The remaining 76 tables are a translation-quality backlog with no known-safe
route.** Both candidate routes are now measured, not merely argued:

- **Route (b), rewriting `_analyze_switch_table`: unnecessary.** Given the
  round-1 oracle's extents, the lifter's *existing* ≥2-arms rule resolves **76
  of 84**. The rule is not too strict; it is handed extents carved out from
  under it. Nobody needs to risk the 484 working dispatches.
- **The pass-ordering fixpoint: converges and buys nothing.** Measured on a
  throwaway probe branch: converges in 4 rounds, adds 38 starts, removes none,
  coverage +274 bytes — and resolves *the same 4 tables* the detector fix
  already resolves alone.

**Why iteration cannot work, which is what makes this final.** The derive
passes are **monotone**. `_pass_tail_jump_targets` skips any target already in
`self._candidates` (only adds); `_pass_demote_interior_seeds` deletes only
candidates whose method is `seed_vtable_thunk` (`functions.py:513`). A start
minted from carved geometry is never retracted, so no number of rounds retracts
it. **Reaching the 76 needs retraction, not iteration.** The one retraction
tried — letting the interior pass judge `tail_jump_target` too — removed 3
starts, missed the target, and cost 32 bytes. Reverted.

**The next thread, for whoever pulls one:** 74 of the 84 tables *are* resynced,
so `_alias_end`'s re-measure should have fired for them and did not. The stall
is inside `_find_function_end`'s stopping rule on alias bodies. That is where
a safe retraction rule would have to come from.

**A regression gate that was retracted, and why it matters.** "No function
smaller than the oracle's" was proposed as a gate and is worthless: it reads
**1,433 on the shipping tree, 1,433 on the fixed tree, 1,433 on the fixpoint
tree**. That is the accumulator legitimately splitting bodies the oracle sees
as one — the accumulator *working*. A gate that fires on a known-good tree
would have waved a real regression through. The gates that do work are coverage
(≥ 1,614,033) and the 84-table count from `switch_arm_causes.py`.

### 2. Converge, THEN regenerate — in that order, and the order matters

All four branches merge cleanly and build green (verified: 73/73 for
walker+padrec+switches against the current gen).

**The trap:** `regenerate.sh` copies `templates/runtime/recomp_types.h` into the
gen tree, and the generated C includes *that* copy. `wt-icall` changes that
header. So merging it and building against an existing gen produces **silently
dead code** — its author hit exactly this and measured it with `nm`.

Therefore: **merge all four first, regenerate once afterwards.** Regenerating
before the merge gets the fixes but not the header, and yields dead hardening
with nothing to show it.

**A red test marks the gap.** `jsrf_gen_header_current` compares the repo's
`templates/runtime/recomp_types.h` against the gen tree's copy byte for byte
and **fails** whenever they differ. It is red on the merged build by design —
verified: it passes against a current gen header and fails against the shared
one, tracking the generated object's real call site in both directions. It goes
green at the regeneration. Do not downgrade it; the state it reports is "the
runtime change you just merged is not in the binary".

It compares bytes rather than the `GENERATION_MANIFEST.txt` sha deliberately:
the manifest records what the header was *at generation time*, which gets a
hand-patched gen wrong, and the file is what decides which macro the compiler
expands. Note also that a configure-time `message(WARNING)` for this already
existed and was read past — by the author of the change it was warning about.
Two hundred lines of configure output is not a gate.

### 3. ONE player session against that binary
The whole day funnels here. With `RECOMP_PAD_RECORD=1` set, play to the point
that has crashed four times — starting to skate in the Corn tutorial.

Four questions, in priority order:
1. **Does the skating crash still happen?** That is G20's verdict.
2. Does `~/Library/Application Support/JSRF/padrec/` get a file, and does
   `[PAD-RECORD] frame=` climb? That is the recorder's verdict.
3. Do the three promoted defaults hold — `[VBLANK-REG] upmirror=on`,
   `[APU-IDLE-OWNER] handoff_guard ON`, `[APU-IDLE-EDGE] reraise` climbing?
4. Is the intro stutter changed?

**Gate the session** on handover §8: `[JSRF-SEQ] now=30`, `[APU-VOICE] on=` in
the **148–453** band, ADPCM `ok>0`. A session reading `on=4–12` is the attract
loop and answers none of the four.

### 4. Then everything else becomes cheap
Once a recording replays, the player is out of the loop and these stop costing
human time: the licensing runs for the three reverted switches (written into
the code beside each), the `METAL_DEFER_SWAP` second pair, G3's four owed
colour-resolve trials, and every frame-time A/B.

### 5. G21 — the texture path *(handover item 0.5, untouched)*
The remaining large win and nobody has started it.

**A correction to the handover first:** it says to read
`[METAL] depth test before the shader: N early, M late` before anything else.
**That line has never been printed by any run in the 782-run corpus** — the
commit that added it (`846217b`) landed nine minutes before the handover was
written. Worse, `hw_early_z()` returns 0 whenever the switch is off
(`nv2a_metal.m:1594`), so with the default the counter *cannot* read anything
but "0 early, N late". Its own comment says as much: the split is only
informative with the switch **on**. So the first step is a run with
`RECOMP_METAL_EARLY_Z=1`, not a read.

What is already known statically: `shade()` samples from a raw
`device uchar*` — `morton()` is a **per-texel bit loop**, DXT1/DXT3 are decoded
by hand, bilinear is hand-written as four `texel()` calls, and `sample_lod()`
can sample two mip levels. Four texture units × 2 levels × 4 texels = **up to
32 software fetches per fragment**, each several byte-wide global loads, with
no texture unit and no sampler cache anywhere. A hardware sampler does this in
one instruction, with DXT decode free.

This is the "everything on the GPU" goal's real content.

#### Sizing it — and the one number nobody has

**Measured:** `sync` is **8.50 ms of a 16.50 ms frame — 51%** — and 93% of that
is the CPU blocked in `[last_command waitUntilCompleted]` at the flip, i.e.
waiting for the GPU. `rest` (the recompiled guest) is flat at ~3.3 ms while the
frame swings. The GPU is the bottleneck.

**Countable from the source**, per fragment, per texture unit:

| | multiplier |
|---|---|
| bilinear (`linear[u]`) | **×4** `texel()` calls — `nv2a_metal.m:637` |
| mip lerp (`min_filter[u] >= 5`) | **×2** `sample_level()` — `:645` |
| texture units | **×4** — `texture_mask & 1/2/4/8`, `:674-676` |
| each `texel()` | a `morton()` **bit loop** (8 iterations at 256×256) or a hand-decoded DXT block, then 4–8 **byte-wide** loads from a raw `device uchar*` |

Worst case 4 × 2 × 4 = **32 software texel fetches per fragment**, each with its
own address arithmetic — for something a hardware sampler does in **one
instruction**, with a texture cache and free DXT decode.

**NOT known, and it is the number that decides the work:** what fraction of GPU
time is sampling. Nobody has profiled the GPU; every counter here measures the
CPU side. So the defensible claim is that the software sampler is two to three
orders of magnitude more work than a hardware one and sits in the stage owning
half the frame — **not** that replacing it returns 8.5 ms. Do not put a
millisecond figure on this until something has measured it.

**The real fix** is `MTLTexture` + `MTLSamplerState`: hardware filtering,
hardware DXT, a texture cache, and `morton()` deleted rather than optimised.

## Rules added today

- **"It does not reproduce in configuration X" is not "it does not happen in
  configuration X".** The freeze never appears in gameplay because in gameplay
  the same defect crashes instead.
- **A failure path correct for what it says it is can still be wrong for what
  reaches it.** `g_esp += 4` is right for a tail jump and catastrophic for a
  switch arm wearing one.
- **Writing the caveat down is not the same as honouring it.** A gate that says
  *controlled run* is not satisfied by an observational sweep with the
  limitation noted in the comment.
- **A watchdog names a symptom.** `[ADX] tick STUCK` is two different failures.
  Do not score a freeze without checking which one it is.
- **Read what a tick counts before calling it untestable.** Three claims died
  this way today, each one grep from refutation.
