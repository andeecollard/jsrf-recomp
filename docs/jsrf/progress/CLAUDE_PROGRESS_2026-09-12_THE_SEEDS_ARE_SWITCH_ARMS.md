# The interior seeds are switch arms, and the loop that makes them was feeding itself

Date: 2026-09-12 (Europe/London)

Started on P1 of `JSRF_GOALS_2026-09-12_CAN_WE_DO_IT.md` -- 77 addresses the
title branches to mid-function, to be expressed as one C function per owner
entered at a label through a selector. P1's premise does not survive
measurement. Those addresses are **switch arms**, and once they are recognised
as such the owner absorbs them, the table resolves, and there is nothing left
for a selector to do.

## What they actually are

Three measurements, static, against the binary.

**Every kept interior seed appears in the image as a four-byte word**, and 146
of the 175 sit in a run of aligned words all pointing into the same owner. The
xref scanner never saw this: `DATA_IMM` records an address taken as an
*instruction operand*, so a table parked in `.text` is invisible to it. That is
why 158 of the 175 were recorded with no incoming reference of any kind.

**143 of the 175 are entries in a table dispatched by an indexed indirect
jump.** Found by scanning the executable sections for the `FF 24 <sib>`
encoding of `jmp dword ptr [reg*4 + disp32]` -- 454 of them -- and measuring
each table the way `engine.resync_jump_tables` does. So are **870 of the 895**
seeds 12 Sep dropped as speculative: right answer, wrong reason.

**The generated C and the binary agree.** `sub_00025040`:

```
00025040  push esi
00025041  mov  esi, ecx
00025043  mov  eax, [esi+0x54]
00025046  cmp  eax, 7
00025049  ja   0x252b7            ; default
0002504F  push ebx                ; <-- pushed here
00025050  push edi                ; <-- pushed here
00025051  jmp  [eax*4 + 0x252bc]  ; switch
00025058  case 0 ...
```

Every arm ends by popping edi, ebx and esi. An arm carved into its own C
function therefore returns with all three changed, which is exactly what
`RECOMP_ABI_CHECK` reports -- and exactly the shape the asset readers show, "a
violation every 50-200 bytes, every one clobbering the same single register".
The generated dispatcher was 24 bytes and eight instructions, ending in
`RECOMP_ITAIL(MEM32(eax * 4 + 0x252BC))`.

## The loop, which was sustaining itself

1. The arm is only ever reached through the table, so a run observes it as an
   indirect-branch target.
2. `icall_targets.json` seeds it as a function start.
3. That start truncates the owner to end at the dispatching jump.
4. Every entry in the table now lies outside `[func_start, func_end)`, and
   `_analyze_switch_table` requires the arms to be inside the function.
5. The jump stays indirect, so the next run observes the same arms.

Nothing in that cycle can break it from inside. The database is persistent, so
it survives every regeneration, and each cycle can only add.

Worth recording separately: `RECOMP_ITAIL` routes through `RECOMP_ABI_CALL`, so
an indirect **tail jump** is checked as though it were a call. A tail jump into
a shared epilogue legitimately restores its caller's caller's registers, so
some fraction of any ABI count measured this way is false by construction. A
counter's trigger again.

## The change

`switch_arm` is a new reference class in the interior-seed pass, and it is
dropped. Both halves of the test are required: the address must be an entry in
a resynced jump table, **and** the `jmp` that dispatches through that table
must lie inside the owner's extent. An address that appears in some table
elsewhere in the image is not an arm of the function it happens to sit in, and
is still kept. `engine.jump_table_sites()` supplies the second half.

Regenerating with `RECOMP_SEED_INTERIOR=1`:

| | control | with switch_arm |
|---|---|---|
| kept interior seeds | 175 | **35** |
| resolved switches in the gen | 293 | **354** |
| unresolved indirect tail jumps | 678 | **615** |
| functions | 9,397 | 9,129 |

`sub_00025040` is now `0x25040..0x252B9` -- whole, one prologue, one epilogue,
the switch resolved to eight `goto`s. That is the shape P1 asked for, reached
by removing the cause rather than by adding a selector.

## At runtime: 131 offenders become 9

The ABI check now records HOW the target was reached, because the three ways do
not mean the same thing and the pooled count could not be read. `RECOMP_ITAIL`
is an indirect *tail jump*: no return address is pushed, the frame belongs to
the jumping function, and a shared epilogue reached that way is supposed to
restore the caller's caller's registers. Routed through `RECOMP_ABI_CALL`, as
it has always been, that reads as a violation every time. 'C' is a direct call,
'I' an indirect call, 'T' a tail jump, and the dedup key is now the pair.

One run to the Corn tutorial under `RECOMP_FAKE_PAD=1`, same instrument, same
machine:

| | control | switch_arm |
|---|---|---|
| ABI offenders | 131 | **9** |
| -- direct call `C` | not partitioned | 4 |
| -- indirect call `I` | | **0** |
| -- tail jump `T` | | 5 |
| RASTER lines | 25 | 30 |
| outcome | SIGSEGV at the end | still running at the 300 s cap |

All nine, in full:

```
[ABI/C] sub_0017D1F8: esp(epilogue never ran)
[ABI/C] sub_0017D231: ebx esi edi
[ABI/C] sub_001816B0: ebx
[ABI/C] sub_0017CAB0: esp(epilogue never ran)
[ABI/T] sub_0007E575: ebx esi
[ABI/T] sub_0007E58D: ebx esi
[ABI/T] sub_0007E594: ebx esi
[ABI/T] sub_0007E5A3: ebx esi
[ABI/T] sub_0007E5AA: ebx esi
```

**No indirect call offends at all.** The five 'T' rows are one shared epilogue
-- five addresses inside 0x35 bytes, every one losing the same two registers --
which is the expected-false class the partition exists to name. Three of the
four 'C' rows are in the CRT at 0x17CAB0/0x17D1F8/0x17D231 and two of them say
`esp(epilogue never ran)`, which is the `__SEH_prolog`/`__chkstk` signature
12 Sep already called expected.

`sub_00011D00` -- `CActBase::recursiveExec1Default`, the one offender in the
exec tree -- **is absent**. The goals document predicted it was a cascade
victim rather than a carved fragment and would resolve with P1. It did.

## What this does NOT show

**The two runs are not comparable on throughput.** 101,017,579 triangles at a
300 s cap against the control's 169,356,768 at its SIGSEGV is not a slowdown
and not a speedup: one run was killed and the other died, and comparing two
builds by wall clock is the same trap as comparing two hosts by wall clock,
which cost a whole fake audio investigation on 12 Sep. An anchored comparison
at equal guest clock is owed. What the run does establish is that the build
reaches the Corn tutorial and outlives the control.

**Nothing here reaches the tutorial jump.** The violations were switch arms,
and on the guest stack an arm's pops match pushes the dispatcher really made --
only the C-level function boundary was wrong. That the count fell is a
statement about the translator, not about the title.

## Reproducing

```sh
RECOMP_SEED_INTERIOR=1 sh diagnostics/jsrf_first_fault/regenerate.sh
```

The pass writes `disasm/interior_seeds.json`; a dropped arm carries a
`dispatch` field naming the jump that reaches it. The control tree from before
this change is preserved as `gen.dropcontrol` and `disasm.dropcontrol`.
