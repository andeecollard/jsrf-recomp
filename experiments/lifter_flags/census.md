# G56 census: every `(_flags` read in JSRF's generated C

Tree: `~/jsrf-build/jsrf-first-fault/gen`, written 2026-09-22 by translator
`4a2547196316b668`. That hash matches HEAD `939c07f`, so the tree is what the
current lifter emits. Reproduce, read-only on the tree:

```sh
/usr/bin/python3 experiments/lifter_flags/census.py --md experiments/lifter_flags/census.md
```

The script finds each site's guest address (the k-th consuming instruction
after the block label, found with capstone), then walks back through the guest
bytes to the nearest instruction that writes a flag the consumer reads. It also
checks jump tables (`jump_tables.json`), incoming xrefs, how the containing
function was detected, and whether the C line can be reached at all. Everything
below the marker is generated; this header is written by hand.

## What the 326 are

**279 of the 326 are not fallbacks.** `_flags` is a real variable in the
rep-string, `lock xadd` and `cmpxchg` lifts, and these sites read the value
those lifts wrote:

- ~200 `sete` into edx/eax, plus 58 `jne`/`je`, all after `repe cmpsd` with
  ECX = 4 or 5. These are GUID/IID compares (QueryInterface and friends),
  and they are correct.
- 17 `jne` after `lock xadd` (refcount release), 1 after `cmpxchg`.

**47 carry the UNRESOLVED FLAGS marker.** None of them is a live defect in
code the owning function runs:

| what | sites | impact |
|---|---:|---|
| inside a switch jump table the disassembler decoded as code (every jp/jnp but one, all 5 LOOPs) | 29 | dead |
| entry of a `tail_jump_alias` split; the owner lifts the same bytes resolved | 15 | only if something enters the alias |
| in an alias copy, a block reached only by an edge from before the split | 2 | dead in the copy |
| after `ret` in an over-long alias body (sub_001533D0), no label | 1 | dead |

The goal note's two guesses do not hold for this tree:

- "~200 sete" are the resolved IID compares above.
- "jp/jnp after FPU compares" is already lifted. `fnstsw ax; test ah, imm;
  jp` resolves to `RECOMP_PARITY8((_fa) & (_fb))` (lifter `test` branch). The
  one parity site in the census, `jp` at `0x00100058`, is the alias entry the
  lead sampled. Its owner, sub_000FFFD0, and six other overlapping aliases
  lift the same `jp` with RECOMP_PARITY8. Class (c) is empty.

## What the resolved sites were hiding (fixed, commit "REPE CMPS/SCAS produce CF")

Three rep compares are followed by something other than je/jne/sete. One of
them is `std::string::compare` (sub_00179AE0; the name comes from the decomp's
symbol table):

```
xor eax, eax ; repe cmpsb (ecx = shorter length) ; je equal ; sbb eax, eax ; sbb eax, -1
```

- **CF was never written**, so the sbb read the xor's CF=0 and every mismatch
  returned +1. "Less" could not come back.
- **A zero count left `_flags` stale.** x86 keeps the xor's ZF=1, so the
  compare reports "equal". `_flags` kept 0, so a comparison involving an
  empty string took the mismatch arm and returned +1.

The other two are harmless. sub_000E12C0 uses the ±1 only as nonzero, with
ECX = 12. The strncmp-like code at 0x17CC70 re-compares the bytes itself.
Every other rep compare loads ECX with a non-zero constant: 4 (233), 5 (21),
12 (1), -1 (1), and `push 4; pop ecx` (the "not in this block" rows).
Whether sub_00179AE0 runs in play is not measured here. Its only static
caller is in the vtable function 0x00179C20 (with alias 0x00179C70).

## Class (b): flags across a function split

Every (b) site is at, or just inside, the entry of a `tail_jump_alias`: a
second copy of a body, starting inside its owner. The flag producer is the
instruction just before the entry (`test`/`cmp` in all 15; one is the x87
`test ah, 5`). Facts:

- **None has a static caller.** 14 of the 15 have no code xref at all in
  `xrefs.json`. `0x000749DC` has one, a `jmp` at `0x00074AEE`, and that jmp
  is inside the owner, which lifts it as `goto loc_000749DC` and joins the
  flags with the published `_zf`. So these bodies are entered only through
  `g_recomp_table` (RECOMP_ITAIL / an indirect call to that exact address),
  if at all. They are fall-through targets inside the owner, not real
  entries.
- The owner lifts the same bytes by fall-through, with the producer in the
  same block, and resolves them. Checked by hand for 0x749DC (`_zf` join)
  and 0x100058 (RECOMP_PARITY8 in sub_000FFFD0).
- Why the 13 aliases with no xref exist at all is open. The likely source is
  a jump decoded out of data. The alias pass records tail-jump targets before
  the jump-table demotion, and several of these addresses are round
  (0x100058, 0x180038, 0x1BD800, 0x1C3800).

**Measure before fixing.** `alias_entries.txt` lists the 15 alias entries
(plus sub_00179AE0 and its caller) in the format
`diagnostics/jsrf_first_fault/instrument_func_hit.py --va-file` takes. If no
count moves in a tutorial and a replay, class (b) is inert in JSRF, and the
right fix is to stop creating these aliases, not to carry flags into them.

Options, cheapest and safest first:

1. **Do not create an alias inside a flag live range.** In `tools/disasm`
   `_build_alias_entries`, refuse an alias whose first instruction reads
   flags that its linear predecessor wrote (or start it at the producer).
   This removes all 15 without touching the lifter. The risk is losing an
   alias that something really dispatches into: exactly what the func-hit
   measurement answers.
2. **Re-materialise a side-effect-free producer at the entry.** When the
   predecessor is `cmp`/`test` and nothing in the gap to the entry writes
   one of its operands (true for all 15: the gaps are `push`/`mov` to other
   registers or to stack slots the producer does not read), emit the producer's
   snapshot at the top of the alias and seed its flag state. The result is
   exact for a fall-through entry. For a dispatched entry it is a guess,
   because on x86 the flags then come from whoever jumped, which is
   unknowable statically. It beats a constant 0, but it can mislead.
3. **Carry flags through globals.** Publish `_fa/_fb/_fas/_fbs/_zf/_cf` plus
   a setter tag to globals at every tail call and indirect tail jump, and
   reload them at every alias entry. This is the only exact option for the
   dispatched case. It costs stores on every tail transfer in 8,700
   functions, and it changes the flag-join machinery. It is a redesign; not
   attempted.

## What regenerating would change

Measured by batch-translating the whole title twice into scratch, with the
same `tools.recomp` invocation regenerate.sh uses (without its post-passes),
once at 939c07f and once at this branch. Neither regenerate.sh nor
~/jsrf-build was touched. The before tree reproduces the 47 UNRESOLVED; the
326 `(_flags` count differs by one, the recovered entry sub_00180020 that the
post-pass adds.

| | before | after |
|---|---:|---:|
| UNRESOLVED FLAGS | 47 | 42 |
| `(_flags` reads | 325 | 320 |
| LOOP sites lifted as counted loops | 0 | 5 (all in jump tables: dead) |
| rep compares that now load the incoming ZF | 0 | 262 |
| rep compares that now write CF | 0 | 8 (in functions that read CF) |

285 changed lines in total, no other differences. So of the 326 sites,
regenerating resolves 5, and those 5 are dead code. The change that matters
is not in the count at all: `std::string::compare` gets its sign and its
empty-string case back, and the 262 zero-count preloads are the same
correctness fix for the IID compares, which always have a non-zero count.
The 42 left are the 17 (b) sites, 24 jump-table sites and the dead line in
sub_001533D0.

<!-- everything below is generated by census.py -->

Total `(_flags` sites: **326**, of which **47** are UNRESOLVED.

| class | sites |
|---|---:|
| (a) producer in function, not understood | 0 |
| (b) producer across a function split | 17 |
| (c) parity from the FPU status word | 0 |
| (d) loop / loope / loopne | 5 |
| (e) other | 25 |
| (r) resolved: _flags really assigned (rep cmps/scas, xadd, cmpxchg) | 279 |

Unresolved sites by class and impact:

| class | impact | sites |
|---|---|---:|
| b | alias copy: owner lifts the same bytes resolved; live only if something enters the alias | 15 |
| b | dead: unreachable in this C function | 2 |
| d | dead: inside jump table 00034034-00034068 | 1 |
| d | dead: inside jump table 00134D30-00134D50 | 4 |
| e | dead: inside jump table 0002730C-000273C0 | 1 |
| e | dead: inside jump table 00027C48-00027C70 | 4 |
| e | dead: inside jump table 00035D34-00035D9C | 1 |
| e | dead: inside jump table 00037FB4-00038058 | 9 |
| e | dead: inside jump table 00038068-0003807C | 3 |
| e | dead: inside jump table 00123B24-00123B44 | 2 |
| e | dead: inside jump table 00134D30-00134D50 | 4 |
| e | dead: unreachable in this C function | 1 |

Per consumer mnemonic and class:

| consumer | a | b | c | d | e | r | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| ja | 0 | 0 | 0 | 0 | 1 | 0 | 1 |
| jae | 0 | 1 | 0 | 0 | 0 | 0 | 1 |
| jbe | 0 | 1 | 0 | 0 | 0 | 0 | 1 |
| je | 0 | 6 | 0 | 0 | 1 | 13 | 20 |
| jg | 0 | 0 | 0 | 0 | 3 | 0 | 3 |
| jge | 0 | 5 | 0 | 0 | 0 | 0 | 5 |
| jl | 0 | 0 | 0 | 0 | 2 | 0 | 2 |
| jle | 0 | 1 | 0 | 0 | 1 | 0 | 2 |
| jne | 0 | 2 | 0 | 0 | 1 | 63 | 66 |
| jnp | 0 | 0 | 0 | 0 | 13 | 0 | 13 |
| jns | 0 | 0 | 0 | 0 | 1 | 0 | 1 |
| jp | 0 | 1 | 0 | 0 | 2 | 0 | 3 |
| loop | 0 | 0 | 0 | 1 | 0 | 0 | 1 |
| loopne | 0 | 0 | 0 | 4 | 0 | 0 | 4 |
| rep-internal | 0 | 0 | 0 | 0 | 0 | 3 | 3 |
| sete | 0 | 0 | 0 | 0 | 0 | 200 | 200 |

Per producer mnemonic (unresolved sites only):

| producer | class | sites |
|---|---|---:|
| add | e | 13 |
| test | b | 10 |
| adc | e | 6 |
| cmp | b | 5 |
| adc | d | 4 |
| none | e | 2 |
| none | b | 2 |
| imul | e | 1 |
| none | d | 1 |
| popfd | e | 1 |
| sahf | e | 1 |
| cmp | e | 1 |

Resolved sites by producer and consumer:

| producer | consumer | sites |
|---|---|---:|
| rep cmps/scas | sete | 200 |
| rep cmps/scas | jne | 45 |
| lock xadd | jne | 17 |
| rep cmps/scas | je | 13 |
| rep cmps/scas | rep-internal | 3 |
| cmpxchg | jne | 1 |

What loaded ECX before each resolved rep compare (a zero count leaves EFLAGS alone):

| ECX | sites |
|---|---:|
| `4` | 233 |
| `5` | 21 |
| `not in this block` | 4 |
| `0xC` | 1 |
| `ecx \| 0xFFFFFFFFu` | 1 |
| `(uint32_t)(-(int32_t)ecx)` | 1 |

## Every unresolved site

| class | site | consumer | generated function | producer | block reached from | function entry kind | impact |
|---|---|---|---|---|---|---|---|
| b | `00014885` | jbe | sub_00014881 | `cmp eax, ebp @0001487E` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00023280` | jle | sub_00023280 | `cmp eax, 1 @0002327D` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00040AAC` | je | sub_00040AAC | `cmp dword ptr [0x251d58], edi @00040AA6` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00041E30` | jge | sub_00041E24 | `test edx, edx @00041E22` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00041E54` | jge | sub_00041E48 | `test ecx, ecx @00041E46` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00041E54` | jge | sub_00041E4C | `test ecx, ecx @00041E46` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00041E54` | jge | sub_00041E50 | `test ecx, ecx @00041E46` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00041E54` | jge | sub_00041E54 | `test ecx, ecx @00041E46` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `000749DC` | je | sub_000749DC | `test eax, eax @000749DA` | 00074AEE/jump | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `000800E1` | je | sub_000800E1 | `test eax, eax @000800DF` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00100058` | jp | sub_00100058 | `test ah, 5 @00100055` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `001000C9` | je | sub_001000C9 | `cmp dword ptr [0x251d54], eax @001000C3` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `00180038` | je | sub_00180038 | `test byte ptr [ebp - 4], 0x20 @0018002E` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `0018010C` | jne | sub_00180038 | `none (no fall-through); block 0018010C entered from 00180002/cond_jump: cmp dword ptr [ebp - 0x10], edi @0017FFF6` | fall-through only | tail_jump_alias | dead: unreachable in this C function |
| b | `0018010C` | jne | sub_00180020 | `none (no fall-through); block 0018010C entered from 00180002/cond_jump: cmp dword ptr [ebp - 0x10], edi @0017FFF6` | fall-through only | recovered entry | dead: unreachable in this C function |
| b | `001BD800` | je | sub_001BD800 | `test al, 1 @001BD7F8` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| b | `001C3800` | jae | sub_001C3800 | `cmp edx, ecx @001C37F8` | fall-through only | tail_jump_alias | alias copy: owner lifts the same bytes resolved; live only if something enters the alias |
| d | `00034060` | loop | sub_00033C50 | `none (no fall-through); block 00034032 entered from no recorded xref` | fall-through only | seed_vtable_thunk | dead: inside jump table 00034034-00034068 |
| d | `00134D44` | loopne | sub_00134B30 | `adc eax, dword ptr [eax] @00134D42` | fall-through only | seed_vtable_thunk | dead: inside jump table 00134D30-00134D50 |
| d | `00134D44` | loopne | sub_00134C87 | `adc eax, dword ptr [eax] @00134D42` | fall-through only | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| d | `00134D44` | loopne | sub_00134CF3 | `adc eax, dword ptr [eax] @00134D42` | fall-through only | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| d | `00134D44` | loopne | sub_00134CFD | `adc eax, dword ptr [eax] @00134D42` | fall-through only | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| e | `00027358` | jne | sub_00026780 | `imul eax, dword ptr [edx], 0 @00027355` | fall-through only | seed_vtable_thunk | dead: inside jump table 0002730C-000273C0 |
| e | `00027C55` | jnp | sub_00027C3E | `add al, byte ptr [eax] @00027C52` | fall-through only | tail_jump_alias | dead: inside jump table 00027C48-00027C70 |
| e | `00027C59` | jl | sub_00027C3E | `add byte ptr [edi], ah @00027C57` | fall-through only | tail_jump_alias | dead: inside jump table 00027C48-00027C70 |
| e | `00027C61` | jnp | sub_00027C3E | `add byte ptr [ebx - 0x60fffd85], dl @00027C5B` | fall-through only | tail_jump_alias | dead: inside jump table 00027C48-00027C70 |
| e | `00027C69` | jnp | sub_00027C3E | `add byte ptr [esi - 0x4dfffd85], ah @00027C63` | fall-through only | tail_jump_alias | dead: inside jump table 00027C48-00027C70 |
| e | `00035D34` | ja | sub_00035640 | `none (no fall-through); block 00035D33 entered from no recorded xref` | fall-through only | seed_vtable_thunk | dead: inside jump table 00035D34-00035D9C |
| e | `00037FC1` | jnp | sub_00037550 | `add eax, dword ptr [eax] @00037FBE` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00037FF1` | jns | sub_00037550 | `add eax, dword ptr [eax] @00037FEE` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00037FFD` | jp | sub_00037550 | `add byte ptr [ebx], ah @00037FFB` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038005` | jp | sub_00037550 | `add byte ptr [esi - 0x4ffffc86], ah @00037FFF` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038009` | jnp | sub_00037550 | `add byte ptr [edx], al @00038007` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038011` | jnp | sub_00037550 | `add eax, dword ptr [eax] @0003800E` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038021` | jl | sub_00037550 | `popfd  @00038020` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038039` | jle | sub_00037550 | `none (no fall-through); block 00038039 entered from no recorded xref` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038041` | jg | sub_00037550 | `sahf  @00038040` | fall-through only | seed_vtable_thunk | dead: inside jump table 00037FB4-00038058 |
| e | `00038069` | jnp | sub_00037550 | `add eax, dword ptr [ebx] @00038065` | fall-through only | seed_vtable_thunk | dead: inside jump table 00038068-0003807C |
| e | `00038071` | jnp | sub_00037550 | `add byte ptr [esi - 0x5cfffc85], bl @0003806B` | fall-through only | seed_vtable_thunk | dead: inside jump table 00038068-0003807C |
| e | `00038079` | jnp | sub_00037550 | `add byte ptr [eax - 0x52fffc85], ch @00038073` | fall-through only | seed_vtable_thunk | dead: inside jump table 00038068-0003807C |
| e | `00123B30` | jg | sub_001237A0 | `adc al, byte ptr [eax] @00123B2E` | 00123A43/cond_jump, 00123A4C/cond_jump | call_target, 3 static callers | dead: inside jump table 00123B24-00123B44 |
| e | `00123B34` | jg | sub_001237A0 | `adc al, byte ptr [eax] @00123B32` | fall-through only | call_target, 3 static callers | dead: inside jump table 00123B24-00123B44 |
| e | `00134D38` | jnp | sub_00134B30 | `adc eax, dword ptr [eax] @00134D36` | 00134B4D/cond_jump, 00134B58/cond_jump, 00134B88/cond_jump | seed_vtable_thunk | dead: inside jump table 00134D30-00134D50 |
| e | `00134D38` | jnp | sub_00134C87 | `adc eax, dword ptr [eax] @00134D36` | 00134C0C/cond_jump, 00134C30/cond_jump, 00134C3B/cond_jump, 00134C44/cond_jump, ... | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| e | `00134D38` | jnp | sub_00134CF3 | `adc eax, dword ptr [eax] @00134D36` | fall-through only | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| e | `00134D38` | jnp | sub_00134CFD | `adc eax, dword ptr [eax] @00134D36` | fall-through only | tail_jump_alias | dead: inside jump table 00134D30-00134D50 |
| e | `0015353F` | je | sub_001533D0 | `cmp eax, 4 @00153539` | 001534AD/cond_jump | tail_jump_alias | dead: unreachable in this C function |
