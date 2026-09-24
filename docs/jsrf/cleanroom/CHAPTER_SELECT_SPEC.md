# Chapter select / chapter jump: functional specification (clean room)

Source of every fact below: the game's own `default.xbe` (JSRF NA, sections as in the
XBE header, `.text` at 0x11000), disassembled with capstone, plus the game's own mission
data files `D:\Media\Mission\mssnCCMM.bin` on the disc dump. Addresses are guest virtual
addresses. "thiscall this=X" means ECX=X; stack arguments are listed in the order the game's
own call sites push them, first stack argument first (the callee reads it from [esp+4]).
Tags: **[certain]** means read directly from code. **[inferred]** means the behaviour is
clear but the name or meaning is my interpretation. **[uncertain]** means the implementer
should verify it in a run.

Terms used below:
- `AM` means the action manager, the pointer stored at 0x22FCE0.
- `VAR[i]` means the global-variable table, dword `AM + 0x7868 + 4*i`.
- `REG[id]` means the object registry, dword `AM + 0x98 + 4*id`.
- `SAVE` means the save object at 0x1EFFB0.
- `SEQ` means the top-level sequence object, whose vtable is 0x1CCEB8.

---------------------------------------------------------------------------------------------

## 0. Core primitives (all [certain])

| Address | Convention | Behaviour |
|---|---|---|
| 0x127C0 | thiscall this=AM, args (index, value), `ret 8` | `VAR[index] = value` (writes `[AM+0x7868+index*4]`). |
| 0x127E0 | thiscall this=AM, arg (index), `ret 4` | returns `VAR[index]`. |
| 0x128C0 | thiscall this=AM, arg (id), `ret 4` | returns `REG[id]` (object pointer or 0). Negative id returns 0. |
| 0x12870 | thiscall this=AM, args (id, obj) | registers: `REG[id]=obj` if 0 <= id < 0x1DF4. Called from the base object constructor 0x12100, which also stores the id at `obj+8`. |
| 0x12890 | thiscall this=AM, arg (id) | clears `REG[id]`. |
| 0x12930 | thiscall this=AM, args (a, b) | swaps `REG[a]` and `REG[b]` and rewrites each object's `+8` id. |
| 0x11BE0 | thiscall this=obj, arg obj | "kill object": sets bit 31 of `obj+4`, unlinks it, clears its registry slot. 0x11C20 does the same recursively for the children (the game always calls both). |

Object header (base constructor 0x12100): `+0` vtable, `+4` flags (bit 31 = dying),
`+8` registry id.

The sequence object is created at 0x12B42 (`call 0x7BC10`) with id 0, so normally
`REG[0] == SEQ`. Its constructor 0x7BC10 sets the vtable to 0x1CCEB8 and `SEQ+0x48 = 0`.
**Recommendation:** do not rely on `REG[0]`. Take SEQ from `this` of the per-frame
function (section B3) and check that its vtable is 0x1CCEB8.

---------------------------------------------------------------------------------------------

## 1. The sequence state machine (SEQ)

- vtable 0x1CCEB8: slot 0 is 0x7D5B0, and slot 1 is **0x7BDD0, the per-frame update**.
  The mission object's vtable slot 1 is its per-frame dispatcher too, which supports this
  reading of slot 1. **[inferred]**
- 0x7BDD0: if `SEQ+0x48 < 0x40`, it calls the handler `table[SEQ+0x48]` from the table at
  **0x20D2B8** (64 dwords). No other code references that table. It then re-reads `SEQ+0x48`.
  For states 0x14..0x2F, except 0x28..0x2B, it calls 0x3A900 (this=SAVE), which increments
  `SAVE+0x10` (the play-time counter). **[certain]**

Relevant states (handler address, then behaviour) **[certain]** unless marked:

| State | Handler | Behaviour |
|---|---|---|
| 0x0D | 0x7C270 | `SEQ+0x48 = VAR[0]`. VAR[0] is the "state to resume" that the game writes before entering a story stage. |
| 0x17 | 0x7C800 | Title-menu result. When `VAR[0x1A8]==0` it switches on `VAR[0x1A3]`. Value 0 (new/continue story): call 0x4EAF0, `VAR[0]=0x1C`, `VAR[3]=0` or 0x5F, state 0x1C. Value 3 (tutorial from the title): `VAR[0]=0x20`, state **0x20**. Values 8..11: `VAR[0]=0x24`, state 0x24. |
| **0x1C** | 0x7C9E0 | **Prepare and start a story stage.** `VAR[0x19A]=1`, `VAR[0x199]=0`, allocates 0xB0 bytes and constructs the stage object with 0x4ECD0 (this=new, args (SEQ, 8, 0)), which becomes `REG[8]`. Then state 0x1D. |
| 0x1D | 0x7CA60 | state 0x1E. |
| **0x1E** | 0x7CA70 | **Story stage running.** Waits until `REG[8]==0`, then state 0x1F. |
| **0x1F** | 0x7CA90 | Stage ended. If `VAR[0x19A]!=0`, it calls 0x4DAA0 (this=0x1FA05C), 0x25740(0x19,0) and 0x25740(0x1A,0), which is the fade clean-up. Then **`SEQ+0x48 = VAR[0x199]`**. |
| **0x20** | **0x7CAE0** | **Prepare tutorial.** See section A1. |
| 0x21, 0x22 | 0x7CBB0, 0x7CBC0 | 0x21 sets state 0x22. 0x22 waits for `REG[8]==0`, then state 0x23. |
| 0x23 | 0x7CBE0 | Tutorial ended. Optional fade (as in 0x1F). Calls 0x3B6A0 (this=SAVE), which restores the real save from the backup. `VAR[2] = SEQ+0x44`, state 0x14 (title). |
| 0x24..0x27, 0x28..0x2B | 0x7CC40 / 0x7CDA0 | Two more "excursions" with the same shape as 0x20..0x23. They use other save stash/restore pairs (0x3B5A0/0x3B6F0 and 0x3B640/0x3B790). |
| 0x13 | 0x7C560 | "Continue from save" (`VAR[0x1CC]==1`): calls 0x4EAF0, then `VAR[6]=SAVE+8`, `VAR[2]=SAVE+4`, `VAR[3]=0x61`, `VAR[0x4D]=SAVE+0xC`, state 0x1C. |
| 0x0E | 0x7C290 | New game (`VAR[0x1CC]==0`): 0x3AED0 (full save reset), 0x39C70 (chapter becomes 1), `VAR[2]=1`, `VAR[3]=1`, `VAR[0x0D..0x10]=-1`, `VAR[0x0D]=6`, state 0x11. |
| 0x3B | 0x7D120 | End of the mission-select menu (object 0x1DEB): when `VAR[0x1BB]==0`, calls 0x4EAF0, `VAR[0]=0x1C`, state 0x1C. The menu (0x137890) had set `VAR[2]`, `VAR[3]`, `VAR[5]` and `VAR[6]` from the (chapter, mission) table at 0x22CB18. |

**The next-state value that makes the sequence prepare a story mission is 0x1C.** Every game
path into a story stage writes `VAR[0]=0x1C` and then sets `SEQ+0x48=0x1C`: 0x7C8E6/0x7C8FC,
0x7C937/0x7C961, 0x7C9A5/0x7C9B0, 0x7D157/0x7D160 and 0x7C5E3. Most of them call 0x4EAF0
first.

0x4EAF0 (cdecl, no arguments) is the per-stage variable reset **[certain]**:
- `VAR[0x4D+i] = i` for i in 0..31.
- `VAR[0xAD+i] = -1`, `VAR[0xED+i] = -1` and `VAR[0x10D+i] = 0` for i in 0..31.
- `VAR[0x151+i] = -1` for i in 0..31.
- `VAR[0x172] = 0`.

---------------------------------------------------------------------------------------------

## 2. Stage object (REG[8]) and mission objects (REG[9] / REG[0xA]) [certain]

**Stage object.** Constructor 0x4ECD0, vtable 0x1CAAF8. Its sub-state is `+0x44`, dispatched
through the table at 0x1FA008 (21 entries). `+0x50` holds the id of the current mission (9).

| Stage state | Handler | Behaviour |
|---|---|---|
| 0 | 0x4EFA0 | Creates the first mission: 0x4E930(`VAR[2]`, `VAR[3]`, 0), which becomes `REG[9]`. Then state 1. |
| 1 | 0x55590 | When `REG[9]+0x48 >= 2`: calls mission method 0x51780(0) to activate it, `+0x50=9`, state 7. |
| 2 | 0x4F010 | Creates the next mission with 0x4E930(`VAR[2]`, `VAR[4]`, 0). Then state 3, which behaves like 1. |
| **7** | 0x4F0E0 | **Steady state while a mission runs.** If both `REG[9]` and `REG[0xA]` exist: state 4 (handoff). If `REG[9]==0`, it reads `VAR[4]`. If `VAR[4]==-1`, it kills itself, which ends the stage, so the sequence sees `REG[8]==0` and goes 0x1E then 0x1F. If `VAR[4]==0x60`, it runs the fade, then state 8, then 9, then 2 (chapter card, then a new mission). Any other value goes to state 2. |
| 4, 5, 6 | 4 = 0x4F060, 5 = 0x4F0B0, 6 = 0x55600 | Within-chapter handoff. 0x55600 moves `REG[0xA]` into `REG[9]` (0x12930(9, 0xA)), activates it, then state 7. |

**Mission object.** Constructor 0x4A910, size 0x1070, **vtable 0x1CAAB0**. It is created by
0x4E930 (as `REG[9]`) or by 0x4E9B0 (as `REG[0xA]`, used for the incoming mission during a
handoff). Both are cdecl with arguments (chapter, mission, flag).

| Offset | Meaning |
|---|---|
| +0x44 | flag argument (1 for missions created by handoff or continue) |
| +0x48 | life stage: 0 = constructed, 1 = data loaded (0x52142), 2 = ready (0x4ACFA, 0x5193C), **3 = activated/live** (0x5194F) |
| +0x4C | chapter C |
| +0x50 | mission M |
| +0x54 | next mission number, written by exit records |
| +0x58 | key `(C<<16) \| M`, the resource key of `mssnCCMM` (see the formatter at 0x3050F: `mssn%1d%1d%1d%1d` built from C/10, C%10, M/10, M%10) |
| **+0x5C** | **mission state**, dispatched every frame by vtable slot 1 = 0x51FC0 through the table at 0x1F9888 (0x77 entries) |
| +0x2B4 | state to return to after an event (0xE or 0xF) |
| +0xFB8 | the current event/trigger record |
| +0x1040 | pointer to the loaded mission data |

The constructor also:
- sets `VAR[4] = -1`;
- zeroes `VAR[0x6D..0x8C]`, `VAR[0x8D..0xAC]` and `VAR[0xCD..0xEC]`;
- sets `VAR[0x2D..0x4C] = -1`;
- calls 0x3AE20(0) (this=SAVE), which clears the mission-scope flags. See section 4.

Mission states used below **[certain]**:

| State | Handler | Behaviour |
|---|---|---|
| 0x0E | 0x5BA00 | Entry, set by the activation at 0x51956. It may start an initial event (0x5B3F0, then 0x57F90); otherwise it goes to 0xF. |
| **0x0F** | 0x5BAD0 | **Normal free-play state.** Polls triggers. On a trigger it records `+0x2B4=0xF` and calls 0x57F90(event type). It also handles save points (state 0x6D) and pause (0x6B when `VAR[0x19B]`). |
| 0x10..0x5C | via 0x57F90 | **Event states.** 0x57F90 maps event types 0xE2..0xF8 and 0x7E to states 0x10, 0x12, 0x14, 0x18 or 0x1A, 0x1E, 0x22, 0x26, 0x2A, 0x2E, 0x32, 0x38, 0x3A, 0x3C, 0x3E, 0x40, 0x42, 0x48, 0x4E, 0x52, 0x58 and 0x5C. An event finishes by writing `+0x5C = +0x2B4` (so back to 0xE or 0xF) and applying the record's flag writes (example: 0x4ADC8..0x4ADE4). |
| 0x5D..0x62 | | Exit to another mission in the same chapter. The new mission is created as `REG[0xA]` (0x539B0 / 0x53A30 / 0x53A60 call 0x4E9B0). State 0x62 (0x53AA0) kills the old mission once the new one's `+0x48 >= 1`. |
| **0x63** | 0x53AF0 | **Exit to the sequence.** Calls 0x4D880 (see below), then state 0x64. |
| **0x64** | 0x53B10 | Calls 0x4D880, sets `VAR[4] = -1`, kills itself (0x11BE0 and 0x11C20). The stage object then ends the stage, and the sequence goes to `VAR[0x199]`. |
| **0x65** | **0x53B40** | **Chapter clear.** See section 3. |
| 0x67 | 0x53C00 | "Continue": creates mission (`+0x4C`, `VAR[6]`, 1) as `REG[0xA]`. |

0x4D880 (thiscall this=mission) sets `AM+0x74=1` (0x12760), calls 0xA63F0 on `REG[0xC..0x2B]`
where present, and sets `mission+0x1038=1`. **[certain; meaning inferred: it resets the
player and actor objects]**

Mission data (`.bin` loaded as a whole) **[certain for the offsets the code uses; the file
parse is inferred]**. The data offsets equal file offsets, and pointers inside the file are
file offsets.

| Offset | Contents |
|---|---|
| +0x194 / +0x198 | exit-record table pointer and count. Each record is 0x44 bytes: `+0` type, `+4` next mission M, `+8` value for `VAR[0x4D]`. |
| +0x17C / +0x180 | trigger/condition records, 0x54 bytes each, interpreted by 0x56990 |
| +0x184 / +0x188 | command records, 0x54 bytes each, interpreted by 0x585E8 |

Layout of a 0x54-byte record:

| Offset | Contents |
|---|---|
| +0 / +4 | prerequisite flag-word pointer and count, tested by 0x39D40 |
| +8 / +0xC | flag-word list pointer and count, written by 0x39D80 |
| +0x10 | opcode |

Exit record types, as dispatched by 0x53FB0 **[certain]**:

| Type | Mission state | Effect |
|---|---|---|
| 0xFB | 0x5D | next mission M, with 0x4D880 |
| 0xFC | 0x5F | next mission M, without 0x4D880 |
| 0xFD | 0x61 | next mission M, with 0x4D880 |
| 0xFE | 0x63 | exit to the sequence |
| 0xFF | 0x65 | chapter clear |
| 0x101 | 0x67 | continue |

Mission numbering, from the disc and the exit tables **[certain from data]**:

| Mission | Role |
|---|---|
| `C96` | Chapter opening. The chapter-clear path loads it: `VAR[4]=0x60` is 96. |
| `C00` | Chapter hub (the Garage). Chapter 5 has no `0500`; its hub is `0510`. |
| `C97` | Continue-from-save entry (`VAR[3]=0x61` is 97; exit type 0x101). |
| `C94` / `C95` | Chapter-opening and save sub-missions. |
| `0101` | The new-game opening (`VAR[2]=1`, `VAR[3]=1`). |
| `20xx`..`29xx` | Challenge missions (the 0x22CB18 table). |
| `34xx` | Roboy's tutorials (chapter 0x22 is 34). |

Chapter 9 is post-game: clearing chapter 8 (mission 0899, exit 0xFF) makes the chapter 9,
and 0x53B40 then takes the credits path.

---------------------------------------------------------------------------------------------

## 3. How the game itself moves to the next chapter: 0x53B40 (mission state 0x65) [certain]

1. 0x39C70 (this=SAVE): `SAVE+4 += 1` (the chapter).
2. 0x3AE20 (this=SAVE, arg 1): clears the **chapter-scope flag set** (16 dwords at `SAVE+0x18`).
3. 0x3AEA0 (this=SAVE, arg 1): clears the **chapter-scope counter** (`SAVE+0x349C = 0`).
4. If the new chapter is 9: fade, `VAR[4]=-1`, `VAR[0x199]=0x34`, mission state 0x66 (credits).
5. Otherwise:
   1. `VAR[2] = SAVE+4` (the new chapter).
   2. `VAR[4] = 0x60` (the next mission is 96, the chapter opening).
   3. Kill the mission (0x11BE0 and 0x11C20).
   4. 0x4D880.
   5. The stage object (state 7) sees `REG[9]==0` and `VAR[4]==0x60`, shows the chapter card
      (states 8 and 9), and creates `mssn(C)96` (state 2).

**Nothing else is reset by the game on a chapter change.** In particular it does not reset
tags, scope-2 flags, global flags, `SAVE+8`/`SAVE+0xC` or the play time. The mission-scope
flags and counter are reset per mission: by the constructor (0x3AE20(0) at 0x4AB7F) and by
0x4C200 (0x3AEA0(0) at 0x4C270).

---------------------------------------------------------------------------------------------

## 4. Save-data layout facts (SAVE = 0x1EFFB0) [certain unless marked]

The persistent block is `SAVE+4 .. SAVE+0x350C` (0xD42 dwords). Evidence: 0x3AED0 zeroes
exactly that range, and 0x3B680 copies it to the backup at `SAVE+0x350C`.

| Offset | Size | Meaning and evidence |
|---|---|---|
| +0x04 | dword | **Chapter.** Getter 0x149D0, increment 0x39C70. Continue copies it to `VAR[2]` (0x7C5A9). |
| +0x08 | dword | **Saved mission number.** Setter 0x39C80, getter 0x149E0. The save point (0x5BBCF) writes `mission+0x50` here; continue copies it to `VAR[6]`. |
| +0x0C | dword | **Saved character/slot value.** Setter 0x39C90, getter 0x39CA0. The save point writes `VAR[0x198]`; continue copies it to `VAR[0x4D]`. [inferred meaning] |
| +0x10 | dword | Play-time counter, incremented each frame by 0x3A900 from 0x7BDD0. |
| +0x14 | dword | 32-bit unlock mask (0x46 after reset). 0x39CB0 sets bit n and also writes `VAR[0x151+n]=-1`, 0x39CF0 clears, 0x39D10 tests. [inferred: unlocked characters] |
| +0x18 | 16 dwords | Flag scope 1, the **chapter flags**. Cleared by 0x3AE20(1). |
| +0x58 | 16 dwords | Flag scope 2. Only a full reset clears it (no caller passes 2 to 0x3AE20). [meaning uncertain] |
| +0x98 | 16 dwords | Flag scope 3, the **global flags**. They survive tutorials: 0x3B420 and 0x3B6A0 copy them across. |
| +0xD8, +0xF8, +0x118 | 8 dwords each | 256-entry graffiti-tag bitsets. 0x39E80 sets a bit in both +0xF8 and +0x118. 0x39EB0 tests +0x118 ("tag sprayed"). 0x39F40 clears +0x118. 0x39F60 copies +0xF8 to +0x118. 0x39E10/0x39E40 set/test +0xD8. 0x3A130 counts +0x118 bits belonging to an area (tag → area table at 0x1F8C38). The reset presets some tags from the tables at 0x1EFD78 and 0x1EFDC8. [set names inferred] |
| +0x138 | 16 dwords | zeroed by reset |
| +0x178 | 0x2800 bytes | set to -1 by reset [meaning unknown] |
| +0x2978.. | | option values (0.85f, 1.0f, `+0x2980=1`, `+0x29F8=0x1E`) [inferred] |
| +0x2A9C.. | | records initialised with times from 0x30570 in steps of 0xE10 [inferred: best-time tables] |
| +0x349C | dword | Scope-1 (chapter) counter. Cleared by 0x3AEA0(1); inc/get/set by 0x3A750, 0x3A780 and 0x3A7B0 with arg 1. |
| +0x34A0 | dword | Scope-2 counter (same functions, arg 2). |
| +0x350C | 0x3508 bytes | Backup copy of the persistent block (0x3B680 writes it; 0x3B6A0, 0x3B6F0 and 0x3B790 restore it). |
| +0x6A14 | 16 dwords | Flag scope 0, the **mission flags** (volatile). |
| +0x6A54 | dword | Scope-0 counter. |

Flag word encoding (0x39B50 test, 0x39BE0 write, both thiscall this=SAVE with one argument):
- bits 0..2: scope (0 = mission, 1 = chapter, 2 = scope 2, 3 = global);
- bits 3..18: index, which must be below 0x200;
- bit 19: value.

0x39D40 (ptr, count) tests all words in a list. **0x39D80 (ptr, count)** writes all words in
a list.

---------------------------------------------------------------------------------------------

## A. RECOMP_CHAPTER_SELECT=<C>[:<M>]

### A0. How a tutorial is chosen [certain]

1. The Garage mission receives an event of type 0xF8, which moves it to mission state 0x40
   (0x4B650). That state opens the menu (0x107320 with 2) and moves to state 0x41 (0x566D0).
2. The menu code at 0x76578..0x765F0, when the pick is a tutorial, writes:
   - `VAR[0x1A3]=3`;
   - `VAR[0x1A4]=<row>`;
   - `VAR[5]=0x22` (chapter 34);
   - `VAR[6]=<tutorial mission>`;
   - `VAR[0x1AA]=3`.
3. Mission state 0x41 (0x566D0), with `VAR[0x1A2]` set and `VAR[0x1AA]==3`, does:
   - 0x126D0(0);
   - `mission+0xFF0=1`;
   - `VAR[0x19A]=0`;
   - **`VAR[0x199]=0x20`**;
   - 0x53FB0(exit index from the event record), which leaves the mission.
4. The stage ends, and sequence state 0x1F sets `SEQ+0x48 = 0x20`. The title menu reaches
   the same state from state 0x17 when `VAR[0x1A3]==3`.
5. **State 0x20, handler 0x7CAE0, is the function the sequence calls to prepare a tutorial.**
   Its only reference is the table entry at 0x20D338; nothing calls it directly. The
   generated C has `sub_0007CAE0`.

What 0x7CAE0 does, which the replacement must **not** do:
1. `VAR[0x19A]=1`.
2. 0x3B680 (this=SAVE), which backs up the save.
3. `SEQ+0x44 = VAR[2]`.
4. `VAR[2]=VAR[5]` and `VAR[3]=VAR[6]`.
5. 0x3B420 (this=SAVE), which stashes the save and resets it for the tutorial sandbox.
6. 0x4EAF0.
7. Creates the stage object with 0x4ECD0 (this=new 0xB0-byte block, args (SEQ, 8, 0)).
8. `SEQ+0x48=0x21`.

State 0x23 later restores the save and returns to the title (state 0x14). The replacement
must therefore leave by state 0x1C, never by 0x21.

### A1. Replacement of 0x7CAE0 (thiscall this=SEQ, no arguments, no return value)

If neither switch is active (and no jump is pending, see section B), run the original
behaviour unchanged. Otherwise, with target chapter C and mission digits M:

1. `VAR[0x19A] = 1` (0x127C0(0x19A, 1)). This matches 0x7CAF7 and 0x7C9F7; state 0x1C writes
   it again.
2. Save chapter reset, mirroring 0x53B40:
   1. Write dword `SAVE+4 = C` (0x53B40 reaches it by incrementing).
   2. Call 0x3AE20, thiscall this=0x1EFFB0, one stack argument 1 (clears the chapter flags).
   3. Call 0x3AEA0, thiscall this=0x1EFFB0, one stack argument 1 (clears the chapter counter).
3. Chapter-1 flags (section C1): when C==1 and M is not 101, apply the listed flag words with
   0x39BE0, thiscall this=0x1EFFB0, one argument per word.
4. Mission choice:
   1. 0x127C0(2, C), so `VAR[2]=C`. 0x53BC3 writes the same variable.
   2. 0x127C0(3, M), so `VAR[3]=M`. Stage state 0 reads `VAR[3]` for the first mission.
5. Call 0x4EAF0 (cdecl, no arguments), as every story entry does before state 0x1C.
6. Optional, to keep the save's character, as in the continue path at 0x7C5CB..0x7C5DE:
   0x127C0(0x4D, dword `SAVE+0xC`). **[uncertain whether needed]**
7. 0x127C0(0, 0x1C), so `VAR[0]=0x1C`.
8. `SEQ+0x48 = 0x1C`.

State 0x1C then:
1. sets `VAR[0x19A]=1` and `VAR[0x199]=0`;
2. builds the stage object;
3. the stage object creates `mssnCCMM` as `REG[9]` from `VAR[2]` and `VAR[3]`;
4. the stage object activates it, and the sequence sits in state 0x1E.

Default M when omitted: **96**. That is the chapter opening the game itself loads after a
chapter clear (`VAR[4]=0x60` at 0x53BCE). `C:0` is the hub, except chapter 5, whose hub is
10. The implementer may check that `mssnCCMM.bin` exists (C and M are two decimal digits
each).

Caveats:
- The save written here is the real save, because the tutorial stash is skipped. The next
  in-game save point (mission state 0x6D, which calls 0x3E930) will persist the new chapter.
- Jumping into a mid-chapter M skips the chapter's earlier scripts: chapter flags they would
  have set, and chapter 7's tag clear (section C2). **[inherent limitation]**
- 0x7CAE0 is also reached from the title menu's tutorial option. The replacement therefore
  also fires there. That is harmless: no stage object exists in either case.

---------------------------------------------------------------------------------------------

## B. RECOMP_CHAPTER_JUMP=<C>[:<M>] (+ _SETTLE=<frames>, default 180; _MARK=1)

### B1. Finding the live mission safely [certain]

Only act when all of these hold:
- SEQ is `this` of 0x7BDD0, `[SEQ] == 0x1CCEB8` and `SEQ+0x48 == 0x1E` (stage running).
- `stage = REG[8]` (0x128C0 with this=[0x22FCE0], arg 8) is non-null, `[stage] == 0x1CAAF8`
  and `stage+0x44 == 7` (no handoff in progress).
- `m = REG[9]` is non-null, `[m] == 0x1CAAB0`, bit 31 of `m+4` is clear (not dying) and
  `m+8 == 9`.
- `REG[0xA] == 0` (no incoming mission).

### B2. "Normal running state"

`m+0x48 == 3` (activated) and `m+0x5C == 0xF` (free play; handler 0x5BAD0). States 0xE and
0x10..0x5C are the entry and event states. States at or above 0x5D are exits, save points
and menus.

Count frames (calls of 0x7BDD0) during which all of B1 and B2 hold for the same object `m`.
Reset the count if `m` changes or a condition fails. When the count reaches SETTLE, fire once.

### B3. Where to run it

At the entry of **0x7BDD0**: SEQ's per-frame update (vtable 0x1CCEB8 slot 1, the only
caller of the state table), on the game thread, before it dispatches `SEQ+0x48`. The
generated C has `sub_0007BDD0`. I assume it runs once per game frame. **[uncertain: verify]**

### B4. Ending the mission the way the game's own exits do [certain]

The game's inline exits to the sequence write the next sequence state, then mission state 0x63:

| Sequence state write | Mission state write |
|---|---|
| 0x4AF0C..0x4AF19: `VAR[0x199]=0xA` | 0x4AF1E: `mission+0x5C=0x63` |
| 0x4C151..0x4C158: `VAR[0x199]=0x38` | 0x4C15D: `mission+0x5C=0x63` |
| 0x57F1A..0x57F27: `VAR[0x199]=0x14` | 0x57F2C: `mission+0x5C=0x63` |

The tutorial exit at 0x5675D writes `VAR[0x199]=0x20` and leaves through the exit record.

So the jump does:
1. Mark the jump as pending, with target C:M.
2. 0x127C0(0x199, **0x20**), which routes the sequence into state 0x20, whose replacement
   (A1) sees the pending jump and applies C:M.
3. Write dword `m+0x5C = 0x63`.

Nothing else is needed. Mission states 0x63 and 0x64 call 0x4D880, set `VAR[4]=-1` and kill
the mission. Stage state 7 sees `REG[9]==0` with `VAR[4]==-1` and kills the stage object.
The sequence goes 0x1E, then 0x1F (with the normal fade clean-up, because `VAR[0x19A]` is
still 1 from state 0x1C), then 0x20, then A1, then 0x1C.

Do **not** clear `VAR[0x19A]` as the tutorial menu does: the game's inline exits leave it
alone. The Garage path also calls 0x126D0(0) (sets `AM+0x54=1`), but the inline exits do not,
so it is not required. **[uncertain what +0x54 does]**

A1 must give the pending jump's C:M precedence over RECOMP_CHAPTER_SELECT and clear the
pending mark.

### B5. MARK (RECOMP_CHAPTER_JUMP_MARK=1)

After A1 has run for the jump:
1. Wait until the sequence is back in state 0x1E and B1 holds for a mission `m` with
   `m+0x58 == (C<<16)|M` and `m+0x48 == 3`.
2. From then on, watch `REG[9]` each frame:
   - When its `+0x5C` is in 0x10..0x5C, note "event seen".
   - The first frame after that on which `+0x5C` is 0xE or 0xF is the end of the first
     event, because events end by restoring `+0x5C` from `+0x2B4`. On that frame, call
     `xbox_PadRecordMark(label)` once, for example `"chapter_jump C:M first event end"`.
   - If `REG[9]` becomes a different mission object (an in-chapter exit, states 0x5D..0x62,
     as when 0196 hands over to 0100), keep watching the new object with the same rule.

---------------------------------------------------------------------------------------------

## C. Chapter-specific facts

### C1. Flags written by the new-game opening, and by tutorials

Roboy's tutorials (`mssn34xx`) run in a sandbox:
- 0x3B420 stashes and resets the save.
- On return, 0x3B6A0 restores the save, keeping only the global flags (scope 3, 0x94..0xD4
  of the stash) and the play time from the tutorial run.
- The 34xx data contains **no** writes to scopes 1, 2 or 3 **[from the data parse]**.

So finishing a Roboy tutorial leaves no chapter state behind. **[certain for the code;
data parse inferred]**

The chapter-1 state that `mssn0100` (the chapter-1 hub) tests is written by the new-game
opening **`mssn0101`**. From its record tables **[inferred from the data parse]**:
- `+1:0` and `+1:1` (trigger 0, prerequisite `-1:0`);
- `-1:1` and `+1:2` (command 50);
- `+2:1` (trigger 12);
- `+3:450`, `+3:455` and `-2:20` (first record of the +0x174 table).

`mssn0100` tests `±1:1`, `±1:11`, `±1:450`, `+3:385`, `+3:386`, `-3:417` and `-3:418`.

Final state after 0101: chapter flags **1:0=1, 1:1=0, 1:2=1**; scope-2 flag **2:1=1, 2:20=0**;
globals **3:450=1, 3:455=1**. As flag words for 0x39BE0:

| Flag | Word |
|---|---|
| +1:0 | 0x00080001 |
| -1:1 | 0x00000009 |
| +1:2 | 0x00080011 |
| +2:1 | 0x0008000A |
| -2:20 | 0x000000A2 |
| +3:450 | 0x00080E13 |
| +3:455 | 0x00080E3B |

Apply them for C==1 when starting at M=96 or M=0, after clearing the chapter flags.

### C2. Graffiti-tag state

Opcode map of the command interpreter (index = op - 0x3A, byte table 0x5B334, jump table
0x5B18C) **[certain]**:

| Opcode | Handler | Effect |
|---|---|---|
| 0x8A | 0x5A73A | clear `SAVE+0x118` (0x39F40) |
| 0x8B | 0x5A751 | restore `SAVE+0x118` from `SAVE+0xF8` (0x39F60) |
| 0x8C | 0x5A768 | per-tag +0xD8 set |

Across all mission files, 0x8A appears only in **`mssn0700`** (command 12). 0x8B appears only
in **`mssn0715`** (22), **`mssn0740`** (19) and **`mssn0775`** (21). **Chapter 7 is the only
chapter whose scripts reset the tag state.** **[data parse, inferred]**

Consequences:
- For a C=7 jump that does not pass through 0700, clear `SAVE+0x118` (call 0x39F40,
  this=0x1EFFB0) to reproduce 0700's effect.
- For later chapter-7 missions, 0x8B restores `SAVE+0x118` from `SAVE+0xF8` at the points
  listed.

Other chapters: sprayed tags persist in `SAVE+0x118` across chapters, and the game's own
chapter change (0x53B40) does not touch them. I did not find a trigger condition that tests
a single tag (condition op 5 at 0x56B9A appears in no mission's +0x17C table). Tag
*counting* is done by 0x3A130, which only the menu/HUD code calls (0x7572F, 0x7880A).
**[uncertain: which chapters gate progression on tag counts]**

Not found in the game's own chapter-transition code: resets of timers other than the scope
counters, of "misc objectives" or of "held items". The only resets are those in section 3.
Mission-scope flags and counter are reset per mission; scope 2 and global flags are never
reset except by the full reset 0x3AED0, which the new-game path at 0x7C3A3 uses. If a fully
clean state is wanted, call 0x3AED0 (this=SAVE) before A1 step 2, then set the chapter. This
is an option, not what the game does on a chapter change.
