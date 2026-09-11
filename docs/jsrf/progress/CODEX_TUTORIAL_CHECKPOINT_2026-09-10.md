# Tutorial checkpoint: opening Corn dialogue

User identified the recomp dialogue as: “This is the GG's Garrage. Hey, where's our pizza?” (user transcription).

Capture: `build-macos/jsrf-first-fault/render-investigation/tutorial-scene-input-working/stderr.log`.
Enabled existing `RECOMP_SCENE_REPORT`; drawOne/drawMany and USB transfer tracing disabled. No game behaviour changes.

Repeated reports at this checkpoint:

- root `040D3A70`, scene root `040FFF40`
- live count 61; scene walk 61 nodes, zero dead-marked nodes
- 47 populated ID slots; IDs 44 and 45 present, 46 absent
- root +7930 / +7934: `FFFFFFFF` / `FFFFFFFF`
- root +7BB0 / +7E48 / +7EC4: zero
- input polls advanced from 10349 to 10949; nonneutral count 308

These are recomp observations, not an established differential. Earlier xemu snapshots were collected at later Corn dialogue/terminal tutorial checkpoints and must not be treated as aligned with this opening line. Allocation addresses are not themselves divergences. The scene reporter prints only the first 24 node records, although it counts the whole bounded traversal and prints populated ID slots.

Next: capture xemu at this exact opening line and compare ID membership and the named root-relative fields before arming a memory watch. In particular, absence of ID 46 is only a candidate until the matching xemu checkpoint is measured.

Input prerequisite: `usb-title-poll-stop` completed via its 120-second bounded stop (not crash). Polls reached 13896, with 187 nonneutral samples, and USB trace showed Start and A. User confirmed controller worked and reached Corn. Earlier draw-traced run stopped at 3480 polls; the cause remains unproven. Disabling draw tracing and restarting changed multiple conditions, so this is not a causal A/B result.

## Claude continuation, 10 Sep 2026

### The ID 46 candidate is dead, from the archived snapshots alone

`snap_corn_talk.bin` already contains the whole `+0x98` id array (7668 slots
end at `+0x7868`, inside the 0x8800 dump), so the matching xemu id set needed
no new run. Decoded:

| checkpoint | live `+87E8` | ids | 44/45/46 |
|---|---|---|---|
| xemu `snap_corn_talk` | 61 | 47 | 44, 45 present; **46 absent** |
| xemu `snap_tutorial_terminal` | 61 | 47 | 44, 45 present; **46 absent** |
| xemu `snap_tutorial_done_a/b/c` | 65 | 46 | 44, 45, **46 present** |
| recomp, this checkpoint | 61 | 47 | 44, 45 present; 46 absent |

The recomp's 47 ids are **element-for-element identical** to xemu's at
`corn_talk`. ID 46 is absent in xemu too during the dialogue and only appears
once the tutorial completes, so its absence here is correct behaviour, not a
divergence. `+7930/+7934/+7BB0/+7E48/+7EC4` also match xemu's `corn_talk`
exactly (`FFFFFFFF FFFFFFFF 0 0 0`).

So the recomp and xemu agree on every quantity measured so far at this
checkpoint. This re-confirms [[jsrf-tutorial-object-state-matches-xemu]] at the
*opening* line specifically, which is what Codex flagged as unverified.

### Heap layout corresponds exactly, modulo one constant

    recomp root      040D3A70   xemu root      00363A70   delta 0x03D70000
    recomp exec root 040FFF40   xemu exec root 0038FF40   delta 0x03D70000

Two independent allocations separated by the identical delta. The allocation
sequence is reproducing; only the arena base differs. (The delta is not
universal -- xemu's `+7FA8` at 0x03C16D44 is high RAM and will not map by it --
so pointers are compared symbolically, by pointee identity, not by adding it.)

### `+0x7FA4` is not a separate tree at this checkpoint

xemu's `m_lpDrawRoot` `+7FA4` reads `0038FF40`, the *same node* as
`m_lpActExecRoot` `+87DC`. So at the tutorial the draw root and the exec root
are one object, and [[jsrf-draw-phase-walks-a-separate-list]] overstates the
case for this field. The genuinely separate structures are
`m_lpDrawSortRoot +7FAC` (xemu: `0165A010`) and the sort bins, of which xemu
has exactly **one** populated (bin 0) during the dialogue.

### Instrumentation added for the differential

- `diagnostics/jsrf_first_fault/main.c` -- `jsrf_object_dump()`, opt-in via
  `RECOMP_OBJECT_DUMP=<dir>`, read-only, bounds-checked, writing
  `objects_NNN.json` once per report up to 128 files.
- `diagnostics/jsrf_first_fault/xemu_objects.py` -- the `/private/tmp` GDB-stub
  tool, brought into the repo and extended to the same schema.
- `diagnostics/jsrf_first_fault/compare_objects.py` -- symbolic differ.

Both sides emit per-object `vtable`, `eACTFLAG`, `draw_child_mask +0C`,
`draw_next +34`, `+38`, `+3C`, `zsort +40`, and the scene-graph links. That is
the first comparison that can distinguish "in the scene graph but not on a draw
list" from "on the list but filtered out".

### Operational note

**Never probe xemu's stub port with `nc`.** Connecting to :1234 *is* a GDB
attach, and attaching stops the VM; a bare `nc -z` froze the guest this
session. Resume with a proper RSP detach (`$D#..`), which is what
`gdb_resume.py` does.

## The paired capture, and what it settled

Both sides captured at the *same* dialogue line, both reading `live=61`,
47 ids, `+7930/+7934=FFFFFFFF`, `+7EC4=0`.

- xemu: `render-investigation/xemu_corn_opening_v2.json` (GDB stub, 128 MB).
- recomp: `render-investigation/recomp_corn_opening_v2.json`, read out of the
  **live process** with lldb (guest RAM maps at host `0x300000000`), so no
  replay was needed for the extra fields. The game survived the detach.
- Per-report series preserved in `render-investigation/tutorial-object-diff-2/`.

### Dead: the draw list, and the eACTFLAG filters

| comparison | result |
|---|---|
| draw-list membership `+3C`, all 47 | **identical** across all three lists |
| `eACTFLAG +04` and `draw_child_mask +0C`, all 47 | **0 differences** |
| `m_dwZSort +14` sort key, all 47 | **0 differences** |
| id set, `live`, `skip_draw`, `draw_mode`, `+7930/7934/7EC4` | identical |

Both CPlayers (44, 45) are on the main draw list `root+0x7FA8` with flags
`0x00805040` on both sides. They are submitted for drawing, and
`drawManyDefault`'s Any/All/None filters are handed identical inputs. So the
missing characters are **not** an InsertDrawList/DeleteDrawList fault and
**not** a flag-filter rejection. That is the hypothesis
[[jsrf-draw-phase-walks-a-separate-list]] proposed, and it is now falsified at
this checkpoint.

The sort machinery is self-consistent, which rules out a decode error: ids 44
and 45 carry keys `0xc184edf0`/`0xc33c94dd`, exactly the negated floats of
their `m_fZ` (-16.616, -188.581), and the key top bytes `0xc1`/`0xc3` = 193/195
are precisely the two bins the recomp had populated.

### Live: uninitialised object fields

`m_fZ +10`, the translation `+18/+1C/+20` and `m_lpZSort +40` differ on ~26 of
the 47 objects. The recomp holds non-zero values; **xemu holds exactly 0**.
104 float-field differences in total.

The sharpest cases are ids 81-86, vtable `0x001C4500`, three children of each
CPlayer, all on the main draw list:

| id | recomp `ty` | xemu `ty` | recomp `+40` | xemu `+40` |
|---|---|---|---|---|
| 81 | -3.495e+26 | 0 | `0x0373ab85` | 0 |
| 85 | -8.312e+25 | 0 | `0x8b8be889` | 0 |
| 86 | -5.226e+26 | 0 | `0x098989fb` | 0 |

`+40` is `CActBase *m_lpZSort`, a pointer; `0x8b8be889` is not an address in
any arena. The values are **stable across four dumps spanning 40 s**, so this
is not a per-frame link state being sampled mid-update -- it is memory nobody
wrote.

### What this does NOT yet establish

That the garbage *causes* the missing characters. Three live confounders:

1. **xemu is running 128 MB** (`-m 128`), not retail 64 MB. A larger arena may
   simply never recycle a block, so its zeros could be an artefact of the
   config rather than what hardware does.
2. Our heap sits elsewhere. Retail RAM wraps at 64 MB (see the mirror comment
   in `src/kernel/xbox_memory_layout.c:3160`), so our root `0x040D3A70` aliases
   physical `0x000D3A70` against xemu's `0x00363A70` -- a different placement,
   hence different reuse.
3. Whether the guest ever *reads* `+18..+20` for class `0x001C4500` is unknown,
   and static reading of the generated C is not admissible evidence here.

Cross-run determinism is untested: run 1 never reached the tutorial, so there
is only one sample of the allocation history.

### The two candidate next measurements

- **Retail-size control.** Reconfigure xemu to 64 MB and recapture. If the
  zeros survive, the divergence is ours; if garbage appears, it is normal and
  this lead dies. Costs one replay, and leaves the oracle retail-faithful.
- **Causal test on our own runtime.** Zero freshly allocated guest blocks and
  observe whether the characters appear. Reversible, entirely host-side, and
  answers the causal question directly rather than by correlation. To be run as
  a labelled experiment, not kept as a fix.

### Tooling added

`diagnostics/jsrf_first_fault/xemu_objects.py` (GDB-stub inventory, was in
`/private/tmp`), `compare_objects.py` (symbolic differ -- resolves pointers to
pointee identity, so the heap-base delta cannot manufacture diffs), and
`jsrf_object_dump()` in `main.c` under `RECOMP_OBJECT_DUMP=<dir>`.

**Save an xemu state at this checkpoint.** Every comparison so far has cost a
full manual playthrough; a save state makes the next one cost seconds.

## The 64 MB control, and where the garbage actually comes from

`render-investigation/xemu_corn_opening_64mb.json`, captured at the same line.

**The memory-size confounder is dead.** At 64 MB xemu's manager is at the
*same* address as at 128 MB (`0x00363A70`, exec root `0x0038FF40`), and the two
configs agree on **233 of 235** of the fields in question. Nonzero float fields:
recomp 106 of 188, xemu **33 in both configs**. Every field where the recomp
holds garbage, xemu@64MB still reads exactly 0.

So the recomp/xemu divergence is real and is not an artefact of the oracle.

### But the mechanism is not a missing zero-fill in our kernel

Three measurements, each of which had to be checked before blaming the
allocator, and together they retire that explanation:

1. `xbox_ReserveAlloc` **already memsets each block to zero**
   (`src/kernel/xbox_memory_layout.c:3509`).
2. The run performs **zero** `MEM_DECOMMIT` and **zero** `NtFreeVirtualMemory`
   calls, so no page is ever recommitted stale. The four
   `MEM_COMMIT ... no-op` lines are all in the first milliseconds, on a region
   that had just been zeroed.
3. The manager at `0x040D3A70` sits inside `NtAllocateVirtualMemory #13`
   (`base_hint=0x040D0000`, 28672 bytes), committed once and never released.

The bytes were therefore written **by the guest**, through JSRF's own heap
recycling memory it had previously used. That happens on real hardware too.

### A wrong turn, corrected

I first read `0x040D3A70` as mirror 0 aliasing physical `0x000D3A70` -- i.e.
the object manager landing on top of the loaded XBE image (which ends at
`0x00288620`), with the x86-looking garbage as image bytes. That was wrong.
`g_memory_size` is 128 MB (mirror 16 is the one that lands on `0x80000000`),
so `0x040D3A70` is ordinary mapped RAM in the 64-128 MB **reserve** region,
above the 58 MB heap at `0x00510000-0x04000000`. No image aliasing occurs.

### What the divergence traces back to

`diagnostics/jsrf_first_fault/main.c:2185` calls
`xbox_EnableSeparateReserveSpace(64 MB)` **deliberately**, to hold JSRF's
1+2+4+8 MB guest-heap reserves outside the retail arena. That is an existing,
documented decision, not a discovery -- but it does mean the title's heap runs
at a different base than on hardware, so its internal free-list history differs,
so different blocks carry stale contents. That is the honest chain from the
config to the measurement.

**Still unproven, and it is the crux:** whether the game *depends* on those
fields being zero. The fields could equally be initialised later by code that
has not run yet at this instant.

### The one experiment that settles it

Disable the separate reserve space (or otherwise put the guest heap where
hardware puts it), rerun to this checkpoint, and read the same fields:

- fields go to zero **and** the characters appear -> cause found;
- fields go to zero and characters still missing -> the garbage was a
  bystander, and the draw path is back in scope with the object state cleared;
- run does not survive -> that is why the reserve exists, and the lead needs a
  different probe.

This is a config change, reversible, and it is a measurement rather than a fix.
Note the comment at that call site warns the arena may not hold the title's
reservations without it.

## The reserve-space experiment: hypothesis falsified

Added `RECOMP_SEPARATE_RESERVE=0` (`main.c`, default unchanged) to keep JSRF's
heap reserves inside the retail arena. Logs in
`render-investigation/reserve-experiment/`.

**The run survives**, and the heap genuinely moved: reserves granted at
`0x00510000 / 0x00C30000 / 0x00D41000` instead of `0x04000000+`, and the object
manager relocated from `0x040D3A70` to **`0x005E3A70`** -- which is exactly
`JSRF_ROOT_VA`, the static root this project measured on 2026-09-04 and still
carries as the fallback constant.

And the garbage did not go away:

| | reserve ON | reserve OFF | xemu |
|---|---|---|---|
| nonzero float fields (of 188) | 106 | **107** | 33 |
| nonzero `+40 m_lpZSort` | 26 | **27** | 10 |

The *values* changed completely (id 81's `+40`: `0x0373AB85` -> `0x0017FE9C`)
but the *quantity* did not. Heap placement is therefore **not** the cause, and
the chain "separate reserve -> different free-list history -> stale fields" is
dead. Object id set, `eACTFLAG`, `draw_child_mask` and `m_dwZSort` remained
identical to xemu throughout (0 differences), so the experiment cost nothing
that was already established.

### What the invariance implies

Ours reads ~106 nonzero float fields under **two very different heap bases**
(`0x040D3A70` and `0x005E3A70`); xemu reads 33 under **two different memory
sizes**. A random recycling accident would not hold that steady across a 60 MB
change of base. That points away from allocation history and towards a
**code-execution divergence**: some per-object initialisation that runs in xemu
does not run, or does not land, in our build -- while the construction and
registration path plainly does, since the registry matches exactly.

The honest counter-reading, which is not yet excluded: our heap has never
matched xemu's placement in *any* configuration (`0x040D3A70` and `0x005E3A70`
against xemu's `0x00363A70`), so a deterministic history difference remains
possible. The invariance is evidence, not proof.

### Next: find the writer, in xemu

The cheap discriminator is to ask the oracle which guest code zeroes these
fields. xemu's GDB stub supports watchpoints, so:

1. at this checkpoint, resolve id 81's address and set a write watchpoint on
   its `+0x40` (and `+0x18`);
2. let it run to the next scene load and record the writing PC;
3. symbolise it with `symbolize.py` and check whether that function executes in
   our build at all.

That is the `observable divergence -> exact writer -> cross-reference` step,
and it needs no new instrumentation on our side.

## Under RECOMP_SEPARATE_RESERVE=0: Corn is visible, and totally still

User observation at the dialogue, reserve-OFF build: **Corn is visible** but
"totally still", and the dialogue text "glitches slightly".

**No clean A/B exists for the visibility change.** Nobody was asked whether the
characters were visible in the reserve-ON run of the same day, so "the reserve
change made Corn appear" is NOT established. It needs a paired observation.

### The scene is frozen -- measured

19 steady-state dumps (`objects_005` .. `objects_023`), spanning ~190 s at the
checkpoint: **every field of every one of the 47 objects is byte-identical.**
Not one flag, link, sort key, transform or address changed.

Meanwhile the runtime is plainly alive: pad polls climbed 29605 -> 33326, GPU
reached draw #608600, ADX ticked to 21393, and the process stayed up. So this
is not a hang, and not the `polls` flatline of
[[jsrf-corn-tutorial-input-poll-stall]] -- polling never stopped.

### The positive control says this may be NORMAL

Two independent xemu sessions (128 MB and 64 MB), each played to this same
line, report **bit-identical** `m_fZ` and `m_dwZSort` for both CPlayers
(`16.61618` / `0xc184edf0`, `188.58150` / `0xc33c94dd`). Two separate manual
playthroughs landing on the same float to the last bit is not plausible for an
animating scene, so **xemu is very likely static at this line too** -- the
title appears to hold the scene while the dialogue box is displayed.

Therefore "47 objects unchanged for 190 s" is **not** established as a fault.
It is an absence measurement whose control currently points the other way.

Caveat on that control: a third xemu capture (`xemu_corn_opening.json`)
predates these fields, so its zeros are missing-key artefacts, not readings.
Only the v2/64mb pair is admissible.

### What would settle it, cheaply

1. **A visual answer from xemu at the same line**: does Corn show idle motion
   there? One look costs nothing and decides whether stillness is the symptom.
2. **A same-session xemu time series**: capture twice ~15 s apart while sitting
   at the dialogue. Differing fields prove the scene advances; identical fields
   prove it does not.
3. **Press A in the recomp** and see whether the tutorial advances. If the
   dialogue will not progress, the stall is in dialogue/event handling rather
   than in animation, which is a different fault from the one being chased.

Note the fields captured are only the first 0x50 bytes of each object; skeletal
animation may live deeper, so "no change here" would not by itself prove no
animation. Another reason not to call this yet.

## Corrected: the CPlayers are NOT frozen, and that is the real lead

User confirmed by eye: **in xemu Corn dances** at this line, and **pressing A in
the recomp does not advance the dialogue**. So the earlier positive control was
misread -- the bit-identical `m_fZ`/`m_dwZSort` across two xemu sessions is
explained by the camera being static while the character animates. Skeletal
animation lives deeper than the 0x50 bytes the object dump captures, exactly
the caveat noted above. Stillness IS the symptom.

An earlier claim in this session -- "CPlayer 44 and 45 completely unchanged" --
was **wrong**, an artefact of diffing only the first 0x400 bytes. Widening to
0x2000 over a 12 s interval on the live process (two lldb reads, guest RAM at
host `0x300000000`) gives:

| object | vtable | changed dwords / 12 s | first offsets |
|---|---|---|---|
| id 44 | `0x001CCFF8` CPlayer | **8** | `0xc94 0x1248 0x1254 0x1260 0x12a8` |
| id 45 | `0x001CCFF8` CPlayer | **136** | `0xc94 0x11cc 0x147c 0x1628 0x162c` |
| id 237 | `0x001CD910` | 5 | `0x1a8 0x3f0 0x638 0x880 0xa78` |
| id 76, 238, 4452, 4625 | | 1 each | |

40 of 47 objects show no change within 0x2000. Both CPlayers share a write at
`+0xc94`, then diverge by 17x. Same class, same draw list, same `eACTFLAG` --
one animates, one barely updates. That asymmetry is the sharpest target yet.

The runtime is unambiguously alive around it: polls 29605 -> 33326, GPU draw
#608600, ADX tick 21393, and 116575 changed dwords in the manager region
including a 471 KB block at `0x0066D00C`.

**Unproven:** which of 44/45 is Corn, and whether xemu shows both CPlayers
updating at a comparable rate. `diagnostics/jsrf_first_fault/xemu_object_delta.py`
(new) measures exactly that over the GDB stub -- same 0x2000 window, same
interval -- so the two sides can be compared with one yardstick. It needs xemu
sitting at this line.

## The decompilation update (fetched 2026-09-10, origin/main 94fa956)

Five commits ahead of the local checkout: four on `SaveData`, one renaming a
method in `ActSequence`. Nothing touching `CPlayer` or the exec phase, so no
direct clue for the animation fault. One real gain, though:

`CActSequence` is the **top-level game-logic state machine**, and its layout
names two fields this session had been capturing raw. `CActBase` ends at 0x44,
so for object id 0 (`eACTID_ACTSEQUENCE`):

    +0x44  m_dwChapterBackup
    +0x48  m_dwNextMethod    -- an INDEX into fSequenceMethods[]

Measured `m_dwNextMethod = 0x1E = 30` in **all four** captures (xemu 128 MB,
xemu 64 MB, recomp reserve-ON, recomp reserve-OFF across 56 dumps / 190 s).
Index 30 is **`WaitEndStoryOrVsMission`**.

Two things follow. The Corn tutorial runs as a *story mission*, not through
`PrepareTutorial` (indices 32-35). And the top-level state machine is **not**
the stall -- both sides sit in the same state, which is the correct one. The
divergence is inside the mission logic below it.

## THE DIVERGENCE, LOCALISED: CPlayer +0xCE0 never updates

Paired measurement, same 0x2000 window, same 12 s interval, both sides at the
Corn dialogue. xemu via `xemu_object_delta.py` over the GDB stub; recomp via
two lldb reads of the live process.

| object | vtable | xemu | recomp |
|---|---|---|---|
| id 44 | `0x001CCFF8` CPlayer | **299** dwords | **8** |
| id 45 | `0x001CCFF8` CPlayer | **506** dwords | **136** |

Not just fewer -- *different places*. In xemu **both** CPlayers update a
contiguous run beginning `+0x9E8` and `+0xCE0, 0xCE4 ... 0xD14+`; in the recomp
**neither does**. The only offset written on all four (our 44, our 45, xemu 44,
xemu 45) is `+0xC94`.

A contiguous dword run of that shape is a transform/animation block. Its
absence is "the character is drawn, correctly placed on the draw list, with
correct flags, and never animates" -- which is exactly what is on screen.

Our id 45 does update a large block at `+0x1628..+0x1CC0` that xemu may also
write; the delta tool only retains the first 16 offsets per object, so
"in recomp but not xemu" is **not** assessable from this capture. Only the
"in xemu but not recomp" direction is sound.

### Candidate writers, found statically

`0x7EC4` appears nowhere in the disassembly (the address is formed from a split
base), but `0xCE0` is small enough to appear literally:

    0x000804A1   lea edi, [esi + 0xce0]     in sub_00080340 (size 0x88E)
    0x00094D21   lea eax, [ebp + 0xce0]     in sub_00094AB0 (size 0x1502)
    0x00094E63 / 0x000951E1 / 0x00095412    also in sub_00094AB0

`sub_00094AB0` touches `+0xCE0` repeatedly through a 5 KB body -- the shape of a
per-frame transform update on `this`. Neither function is in the
decompilation's 1332 named symbols.

**Both are present in the current generated sources** (`recomp_0002.c`), so
there is no translation gap. An earlier reading of `grep -rl` output suggested
they existed only in `.before-*` backups; that was wrong, and checking the
current files directly disproved it.

### The next measurement

Whether `sub_00094AB0` and `sub_00080340` **execute** at the tutorial in our
build. Static presence is not execution, and this is precisely the claim
CLAUDE.md forbids inferring from generated C. A hit counter on those two VAs,
in the style of `instrument_draw_one.py`, answers it directly:

- never called -> the fault is upstream, in whatever should dispatch them;
- called but `+0xCE0` unchanged -> the fault is inside, and RECOMP_MEM_WATCH on
  one CPlayer's `+0xCE0` finally has a specific target worth arming.

### Tooling caveat, paid for twice

**Any script that halts xemu must resume it on every exit path.** An ad-hoc
probe raised an exception between `\x03` and `$c`, leaving the VM stopped with
a dead connection; the stub then refuses new connections (a stale CLOSED socket
beside the listener) and xemu has to be killed and the checkpoint replayed.
`xemu_object_delta.py` was hardened for this; the one-off script was not.
Never write one of these without try/finally around the resume.

## ROOT CAUSE CHAIN: CPlayer +0xE54 bit 0 is never set

Measured with `func_hit_probe.c` + `instrument_func_hit.py` (new), at the Corn
checkpoint, `RECOMP_SEPARATE_RESERVE=0`, logs in
`render-investigation/callee-hit/`.

### The outer function runs; every callee that would animate has stopped

Two consecutive reports at `live=61`:

| function | calls | |
|---|---|---|
| `sub_00094AB0` | 632 -> **1072** | climbing, +440/report |
| `sub_00080340` | 632 -> **1072** | lockstep with it |
| `sub_000606C0` | 386 -> **386** | **frozen** |
| `sub_00060730` | 386 -> **386** | **frozen** |
| `sub_00060930` | 8 -> **8** | **frozen** |
| `sub_000651E0` | 5 -> **5** | **frozen** |

`CActMan::drawOne` counted 4165 as a positive control, so the instrument works.
The `+0xCE0` block is never written by `sub_00094AB0` itself -- it is passed as
a **pointer argument** to those callees, and none of them is being called.

### The gate, read directly

    0x00094D08  test byte ptr [ebp + 0xe54], 1
    0x00094D0F  je   0x94d8e         <- skips the whole animation block
    0x00094D11  cmp  dword ptr [ebp + 0x1144], ebx
    0x00094D17  jne  0x94d8e
    0x00094D19  ...                  <- the +0xCE0 work

Read out of the live process (lldb, guest RAM at host `0x300000000`):

| object | address | `+0xE54` | bit 0 | `+0x1144` |
|---|---|---|---|---|
| CPlayer 44 | `0x03536B20` | `0x00000000` | **0** | `0x00000000` |
| CPlayer 45 | `0x03542D00` | `0x00000000` | **0** | `0x00000000` |

The gate fails at the **first** test. `+0x1144 = 0` would have passed the second.

### Who sets it

`+0xE54` is a bitfield (bits 1, 2, 4, 8, 0x10, 0x20, 0x40 tested elsewhere).
Of its 57 references, most writes clear it; the one that computes a value is
the tail of `sub_0009D030`:

    0x0009D0A4  call sub_000A0260
    0x0009D0AA  mov  dword ptr [esi + 0xe54], eax     <- the flag word IS the return value
    0x0009D0B2  ret

So the chain is:

    sub_000A0260 returns a flag word
      -> sub_0009D030 stores it at CPlayer +0xE54
        -> bit 0 gates the block in sub_00094AB0
          -> which calls sub_000606C0 / 00060730 / 00060930 / 000651E0
            -> which write CPlayer +0xCE0, the block xemu updates and we do not

### Next, and it is narrow

1. Count `sub_0009D030` and `sub_000A0260` at the tutorial. If `sub_0009D030`
   never runs, `+0xE54` keeps whatever the clearing writes left. If it runs,
   `sub_000A0260` is returning 0 where the title returns bit 0 set.
2. Capture `sub_000A0260`'s return value; it is a leaf-ish flag computation and
   the likeliest single point of failure.
3. Confirm against xemu that `+0xE54` bit 0 is **set** there. Not yet measured:
   the xemu instance was relaunched without `-s`, so this direction is inferred
   from "Corn dances in xemu", not read. **It should be read before the fix.**

### Tooling constraints learned

- **Do not instrument mid-function `loc_` labels.** Counters at `00094D20`,
  `00094D3F`, `00094D47`, `00094D5A` wedged the guest on the SEGA screen
  (`drawOne` stuck at 12, `polls=1`, no GPU draws). Function-entry labels are
  safe; the same seven entry counters ran fine. Isolated by bisection.
- The `[PAD-POLL]` stall reproduced once mid-load (`polls=1670` flat,
  `nonneutral=121`, `not_connected=0`) and cost a run. It is intermittent and
  unrelated to the controller -- see [[jsrf-corn-tutorial-input-poll-stall]].
- `jsrf_func_hit` only creates a row on first hit, so "never called" shows as an
  **absent row**, not `calls=0`. Always read it beside the `drawOne` control.

## The chain resolves to a wrong character index at CPlayer +0x9E4

`[JSRF-GATE]`, a cheap two-object sampler in `jsrf_scene_report`, at 3 s
resolution (`render-investigation/gate-trace-2/`). User reports Corn "moved
slightly then stopped".

### ids 44/45 are re-pointed at NEW objects mid-scene

| id | object | `+0xE54` observed | bit 0 |
|---|---|---|---|
| 44 | `0x03627400` | `0` -> `0x00002000` -> **`0x00000003`** | **1** |
| 45 | `0x036307E0` | `0x00000002` -> **`0x00000003`** | **1** |
| 44 | `0x03536B20` | `0x00000000` throughout | 0 |
| 45 | `0x03542D00` | `0x00000000` throughout | 0 |

The **first** pair reaches bit 0 set and animates -- that is the brief movement
on screen. Then ids 44/45 point at a **second** pair whose flag never leaves 0,
and motion stops. `sub_0009D030` was separately measured running for that
second pair (`this=03536B20` x3, `03542D00` x2), so the setter is not skipped.

### The flag is a static table lookup, and the table is fine

`sub_000A0260` is 17 bytes:

    mov eax, [esp+4]              ; index
    lea eax, [eax + eax*2]        ; x3
    mov eax, [eax*8 + 0x20E924]   ; table at 0x0020E924, stride 24
    ret 4

At the call site the index is `edi`, also stored to `[esi+0x9E4]`, so
**CPlayer +0x9E4 is the index**. Read live:

| object | `+0x9E4` | `table[idx]` first dword |
|---|---|---|
| 44 `0x03536B20` | **61** | `0x00000000` |
| 45 `0x03542D00` | **61** | `0x00000000` |
| old 44 `0x03627400` | 85 | `0x00002000` |
| old 45 `0x036307E0` | 179 | `0x00000000` |

The table holds 39 nonzero entries of 341 and `table[85]=0x2000` matches the
`+0xE54=0x00002000` actually observed on the old object, so the lookup works.
**The fault is the input:** both CPlayers carry index 61, which maps to a zero
entry, so the flag is legitimately zero and the gate legitimately closes.

Two different characters sharing one index is the anomaly. The old pair had
distinct indices (85, 179).

### What is NOT the problem

- Not struct layout. Offsets come from the decompilation's real `CActBase` /
  `CActMan` headers and the paired xemu capture matched field-for-field across
  all 47 objects.
- Not the table contents, per above.
- Not `sub_000A0260`, which computes exactly what it is asked to.
- Not the draw path, flags, id registry, heap placement or `CActSequence`.

### The next measurement, and it is the one still owed

**Read xemu's CPlayer `+0x9E4` and `+0xE54` at this checkpoint.** Everything
above says index 61 is wrong because Corn animates in xemu -- inference, not
measurement. If xemu also shows 61 then the index is correct and the fault is
elsewhere again. This needs an xemu launched with `-s`; the last instance was
started from the app bundle without it.

Then: find who writes `+0x9E4`, and why the second pair of CPlayers is created
at all -- whether that re-pointing also happens in xemu is unmeasured.

`+0xE54` is not simply the stored table value; it is modified afterwards
(old 44 went `0x2000` -> `0x3`), so treat the table result as an initial value.

### Correction: the "first pair" are a different class, and deleted

Read live, all four objects' first 16 bytes:

| object | vtable | eACTFLAG |
|---|---|---|
| current 44 `0x03536B20` | `0x001CCFF8` | `0x00805040` |
| current 45 `0x03542D00` | `0x001CCFF8` | `0x00805040` |
| old 44 `0x03627400` | **`0x001C4390`** | **`0x80805040`** |
| old 45 `0x036307E0` | **`0x001C4390`** | **`0x80805040`** |

Bit 31 is `eACTFLAG_DELETE`. So the earlier pair are **not** CPlayers at all --
different class, and dead. The `[JSRF-GATE]` sampler caught id slots 44/45
during loading, while they still held stale objects of another class, before
the real CPlayers were installed.

**So the section above overstates its case.** ids 44/45 are not "re-pointed at
new objects mid-scene" with the second pair suspect; the first pair are
loading-phase leftovers. Their `+0xE54` bit 0 and their indices 85/179 say
nothing about CPlayer, because `+0xE54` and `+0x9E4` are class-specific
offsets. Only the current pair is admissible.

**Not a vtable fault:** the live CPlayers' vtable `0x001CCFF8` and flags
`0x00805040` match the xemu capture exactly, on both objects.

What survives: the live CPlayers carry `+0x9E4 = 61` for **both**, and
`table[61]` first dword is `0x00000000`, so `+0xE54` is zero and the animation
gate closes. Whether 61 is wrong is **still unmeasured** -- it needs xemu's own
`+0x9E4`, which requires an instance launched with `-s`.

## FALSIFIED: the +0xE54 gate is closed in xemu too

Read from xemu at the Corn checkpoint (`-s` instance, guest resumed cleanly):

| side | id | address | vtable | eACTFLAG | `+0x9E4` | `+0xE54` | bit0 | `+0x1144` |
|---|---|---|---|---|---|---|---|---|
| xemu | 44 | `0x03BFEB00` | `001CCFF8` | `00805040` | **233** | `0` | **0** | `0` |
| xemu | 45 | `0x03C0ACE0` | `001CCFF8` | `00805040` | **234** | `0` | **0** | `0` |
| recomp | 44 | `0x03536B20` | `001CCFF8` | `00805040` | **61** | `0` | 0 | `0` |
| recomp | 45 | `0x03542D00` | `001CCFF8` | `00805040` | **61** | `0` | 0 | `0` |

`xemu table[233]` and `table[234]` first dwords are **both `0x00000000`**.

**So xemu's animation gate is closed exactly as ours is, and Corn dances there
anyway.** `test byte [ebp+0xE54], 1` cannot be what drives character animation.
The chain recorded above -- sub_000A0260 -> +0xE54 bit 0 -> sub_00094AB0's
callees -> +0xCE0 -- is **dead**.

**The unexamined assumption that killed it:** that every `lea [reg+0xce0]` site
operates on a CPlayer. `sub_00080340` uses `[ebx+0xce0]` and `sub_00094AB0`
uses `[ebp+0xce0]`; those bases were never shown to be the same object, or to
be a CPlayer at all. A static offset match is not an object identity.

This is the third hypothesis measured to death today, after ID 46 and heap
placement. Recording it so it is not rebuilt.

## What survives, and is now the lead: +0x9E4

The same read produced a real, two-sided divergence:

- xemu: `+0x9E4` = **233** and **234** -- distinct per character, and **both are
  populated object ids** in the registry (233 and 234 are in the 47-id set).
- recomp: `+0x9E4` = **61** for **both** CPlayers -- and **61 is not a populated
  id at all**; our id set runs `... 12, 16, 44, 45, 76 ...`.

So `+0x9E4` reads like a reference to an associated object. xemu's CPlayers each
reference a distinct live object; ours both reference the same non-existent id.
This is measured on both sides, not inferred.

Note `+0x9E4` is written by `sub_0009D030` (`mov [esi+0x9E4], edi`, `0x0009D09E`)
from the same `edi` it passes to `sub_000A0260`. That function was measured
running for both CPlayers (`this=03536B20` x3, `03542D00` x2), so the wrong
value is reaching it, or it is called with a wrong argument.

**Next:** find where `edi` comes from at that call site, and what ids 233/234
are in xemu (their vtables are in `xemu_corn_opening_v2.json`: 233 is
`0x001D77B8`, 234 is `0x001D7778`). Check whether our objects 233/234 exist and
what they are -- they were in our id set too.

## RESOLVED to a single wrong input: CPlayer +0xE6C

The `+0x9E4` index is built entirely from guest state and one image-resident
table. Decoding `sub_000A0120` from raw bytes (the listing is misaligned there,
showing overlapping instructions):

    eax = [this + 0xE70]                     ; selector
    test eax, eax
    eax = arg * 3
    if selector == 0: return arg*96 + 0x2104A0
    else:             return arg*96 + 0x2104D0

`0x2104D0 - 0x2104A0 = 0x30` and the stride is 96, so this is **one table of
96-byte records** at `0x002104A0`, the selector choosing a half-record. Hence

    [this+0x9E4] = *( 0x2104A0 + [this+0xE6C]*96 + (0 or 0x30) + 0xC )

Confirmed live in xemu, both objects, exactly:

| side | id | `+0xE6C` | `+0xE70` | record VA | `+0xC` | actual `+0x9E4` |
|---|---|---|---|---|---|---|
| xemu | 44 | **170** | 0 | `0x00214460` | 233 | 233 |
| xemu | 45 | **171** | 0 | `0x002144C0` | 234 | 234 |
| recomp | 44 | **128** | 0 | -- | 61 | 61 |
| recomp | 45 | **128** | 0 | -- | 61 | 61 |

### The table is correct in our build -- verified against the XBE itself

Extracted from `default.xbe` (section 7, VA `0x001EB760`, file offset
`0x200D40`; XBE section headers put VirtualAddr at `+0x04`, VirtualSize `+0x08`,
RawAddr `+0x0C` -- an earlier attempt used `+0x0C/+0x10/+0x14` and produced
garbage that looked like a runtime-populated table):

    XBE record[128] +0xC = 61     <- exactly our value
    XBE record[170] +0xC = 233    <- exactly xemu's
    XBE record[171] +0xC = 234

**XBE static bytes == xemu runtime bytes over all 6144 fetched.** The table is
image-resident, never rewritten, and identical on both sides.

### So the entire chain is correct, and the input is wrong

Not the table. Not `sub_000A0120`. Not `sub_000A0260`. Not the vtable, layout,
draw path, id registry, heap placement or `CActSequence`. The lookup faithfully
returns 61 because it is asked for slot **128**, and 61 is genuinely what slot
128 holds.

**The fault is that both CPlayers carry `+0xE6C = 128` where xemu gives them
170 and 171** -- distinct, adjacent, plainly per-character. Two characters
sharing one slot is the anomaly. `+0xE70` matches (0) on both sides, so the
selector is not involved.

**No emulator boundary is crossed anywhere in this chain** -- it is three guest
functions and an XBE-resident array, so this stays a guest-code differential
until `+0xE6C`'s writer is found.

### Next: which of nine functions writes it

Twelve store sites, in nine distinct functions:

    sub_0007FF90  sub_000836F0  sub_00098DF0  sub_0009A170  sub_0009B2D0
    sub_0009B3E0  sub_0009B4E0  sub_0009E910  sub_000A0400

`sub_0009E910` is notable: it is the **caller of `sub_0009D030`**, the function
that consumes `+0xE6C`.

Instrument these nine **at entry** with `jsrf_func_arg` to capture `this`, and
see which run for the CPlayer addresses. Entry sites only -- mid-function `loc_`
counters wedge the guest (see the tooling note above).

## Origin traced to CPlayer +0x11C

`sub_0009E910` runs for both CPlayers (`this=03536B20` and `03542D00`, 245 calls
each and climbing; it also ran 174 times each for the two dead objects, then
stopped). At `0x0009EBC3` it does a straight copy:

    0x0009EBC3  mov eax, [esi + 0x11C]
    0x0009EBD6  mov [esi + 0xE6C], eax
    0x0009EBDC  mov [esi + 0x9E4], 0xFFFFFFFF   ; reset before the lookup

So `+0xE6C` is simply `[this+0x11C]`. Measured, both sides:

| | `+0x11C` | `+0xE6C` | `+0xE70` | `+0x9E4` |
|---|---|---|---|---|
| xemu 44 | **170** | 170 | 0 | 233 |
| xemu 45 | **171** | 171 | 0 | 234 |
| recomp 44 | **128** | 128 | 0 | 61 |
| recomp 45 | **128** | 128 | 0 | 61 |

**The whole chain is faithful on both sides.** Every stage -- the copy, the
selector, `sub_000A0120`'s record arithmetic, `sub_000A0260`'s field read, the
XBE table -- does exactly the same thing in the recomp as in xemu. The single
divergent quantity in the entire path is `CPlayer+0x11C`: **128 (`0x80`) for
both characters, against 170/171 (`0xAA`/`0xAB`) in xemu.**

Two distinct characters sharing one slot is the defect; 61, the closed gate and
the missing `+0xCE0` writes are all downstream consequences.

### Why the static search stops here

`+0x11C` has **440 references** and 20+ store sites across many classes -- it is
a common offset, not a CPlayer-specific one, so the disassembly cannot say which
store belongs to this object. Runtime attribution is required.

`RECOMP_MEM_WATCH` now has the specific target it always lacked: 4 bytes at
`CPlayer+0x11C` (e.g. `0x03536C3C` when id 44 sits at `0x03536B20`, which has
been stable across runs). See [[jsrf-mem-watch-cannot-run-on-gameplay-build]]
for the recorded blocker -- re-verify it rather than trusting it, since the
build has changed a great deal since that note.

### Instrumentation cost is itself a hazard -- measured

`jsrf_func_arg` originally linear-scanned up to 192 entries on every call. With
nine hot sites instrumented the pad poll stalled **within the first ten seconds**
(`polls=475`, flat, `nonneutral=262`, pad opened normally); with one site and a
bounded scan the same build ran to the tutorial with `polls` climbing past 6000.
The stall arrives sooner the heavier the probe set. Repeated "controller not
registering" reports during this session were this effect, not hardware and not
purely the pre-existing stall.

**Rule:** instrument one site at a time, keep per-call work O(1)-ish, and treat
a sudden early poll stall as a signal that the probe set is too heavy.

## RECOMP_MEM_WATCH confirmed unusable on this build (A/B proven)

Armed on `CPlayer 44 + 0x11C` (`0x03536C3C`) it announced
`[MEM-WATCH] armed source=guest va=0x03536C3C length=0x4 ram=... aliases=on`
and the guest died at t=5 s:

    SIGNAL: SIGSEGV, HOST FAULT ADDRESS 0x00000003FFFFFFF8
    -> guest 0xFFFFFFF8 (host base 0x300000000); ESI=FFFFFFF0 EDX=FFFFFFF0

**Control, identical binary, `RECOMP_MEM_WATCH` unset: zero faults**, polls to
8534, reached `live=61` within 70 s. So the watch is causal, not incidental.

The store paths are symmetric (`*_mw_ptr = _mw_value` when disabled versus
`recomp_mem_watch_guest_store()` which also stores), so the wild address is not
produced by the watch's own arithmetic. The mechanism is almost certainly cost:
the watch puts a function call on **every** guest store, a far heavier
perturbation than the counters that already destabilised runs tonight.

**This confirms [[jsrf-mem-watch-cannot-run-on-gameplay-build]] rather than
retiring it.** An earlier claim in this session that the blocker was stale --
based on seeing the hook present in the generated `recomp_types.h` -- was wrong.
Presence of the hook is not the same as the build tolerating it.

## sub_0007EA00 eliminated, and the signature moves upstream

`sub_0007EA00` sets `+0x11C` from `[this+0x10F4]`, gated on
`[this+0xE50] == 0x1B`, then calls `sub_0009E910`. Measured on both sides:

| | `+0xE50` | `+0x10F4` | `+0x11C` | `+0x9E4` |
|---|---|---|---|---|
| xemu 44 | **23** | 0 | 170 | 233 |
| xemu 45 | **25** | 0 | 171 | 234 |
| recomp 44 | **15** | 0 | 128 | 61 |
| recomp 45 | **15** | 0 | 128 | 61 |

The gate wants 27; nobody has it, and `+0x10F4` is 0 on both sides, so this path
never ran for these objects. Eliminated.

**But `+0xE50` carries the same signature as everything downstream:** two
distinct values in xemu (23, 25), one repeated value here (15, 15). Every
per-character identity field found so far is distinct-per-character in xemu and
identical-across-characters in the recomp.

That is the shape of the two CPlayers being **constructed with the same
character identity**, rather than of one field being miswritten. The chain
`+0xE50 -> +0x11C -> +0xE6C -> +0x9E4 -> table -> +0xE54` is then all
downstream of a single wrong assignment.

### Next

`CPlayer` is constructed by `sub_0007E830` (the only site storing vtable
`0x001CCFF8` besides `sub_00084200`), called from **`sub_00080320`**. The
constructor takes no visible identity argument in its prologue, so the identity
is either already on the object or applied by the caller.

Find what writes `+0xE50`, or instrument `sub_00080320` at entry (with `this`
capture, one site at a time -- see the probe-weight rule) to see what it
assigns to each of the two CPlayers.
