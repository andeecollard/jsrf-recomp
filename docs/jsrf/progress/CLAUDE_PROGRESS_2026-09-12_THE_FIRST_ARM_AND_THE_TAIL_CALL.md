# The switch-arm loop was displaced by one entry, and the unclamped walk was running away

Date: 2026-09-12 (Europe/London)

Two defects found by measurement after
`CLAUDE_PROGRESS_2026-09-12_THE_SEEDS_ARE_SWITCH_ARMS.md`. The first is a hole
in that note's own fix. Neither was visible from the numbers it reported,
because both of them make the *owner* wrong rather than the arm.

## 1. The first arm defeats the owner-bounded test

`_switch_arm_of` asks whether a table entry's dispatching `jmp` lies inside the
owner's extent. MSVC makes case 0 the dispatch's own fall-through, seven bytes
after it -- so a switch's FIRST arm sits exactly on a boundary of its own
making. Seeding it clamps `_find_function_end`, the dispatching function ends
precisely at the jump, and two things follow:

* the first arm is interior to nothing, so the interior test never examines it;
* every later arm in the same table is judged against an owner that *begins*
  after the dispatch site, so `lo <= site < hi` is false for those too.

Measured across all 422 resynced tables:

| | count |
|---|---|
| tables the raw `FF 24` scan finds with >=3 in-section entries | 422 |
| tables the engine actually resynced | 422 |
| tables the engine missed | **0** |
| resolve for the lifter (>=2 leading arms inside the dispatcher) | 261 |
| dispatching function ends exactly at the jump | **137** |
| ...of which truncated by a seed on the fall-through | **133** |

So `resync_jump_tables` was never at fault: it found every table. 26 of the 28
still-carved arms were classified `speculative` rather than `switch_arm` purely
because the owner handed to the test began after the dispatch.

`_fallthrough_arm_of` looks backwards a short distance instead -- the dispatch
is the instruction the arm falls out of, so it is a handful of bytes behind it
whatever the boundaries say -- and the pass now considers a seed that is a first
arm even though nothing contains it.

## 2. The unclamped walk runs away, and tail calls are why

`functions.json` is healthy: its largest body anywhere is **11,124 bytes**.
The runaway is at one call site. `_pass_demote_interior_seeds` calls
`_find_function_end(start, None, sec_end)` -- `next_func=None`, deliberately,
because it wants the extent the sweep would find without any seed. It is the
only unclamped call in the tree.

| percentile, 6,566 primary starts | clamped | unclamped |
|---|---|---|
| p50 | 90 | 94 |
| p99 | 1,974 | **45,312** |
| p99.9 | 4,639 | **270,906** |
| max | 11,124 | **273,162** |

The code states the invariant it is breaking, at `_find_function_end`:

> `upper` is already clamped to the next known function start, so a target
> inside these bounds is internal rather than a tail call.

True at the builder. At the pre-pass `upper` is the whole 1.5 MB `.text`, so
every forward tail call in the image satisfies it. Once `max_target` is past a
`ret` the walk cannot stop; it decodes into the next function, whose own tail
calls ratchet it further, and the chain ends only where the decode dies.

In **83 of the 83** worst escalations the culprit is an unconditional
`jmp rel32` to an address already in `functions.json`. Zero mis-decoded
branches. Zero jump-table misreads. `sub_0013AEB0` is the clean proof: 95 bytes
becoming 271,018, and it both *calls* `0x13C480` and tail-jumps to it twice, so
the target is unarguably a callee. Its terminus `0x0017D15A` is not a
terminator -- it is a byte in a data run where a misaligned decode chain died.

The fix states the invariant rather than relying on the caller to supply it: an
unconditional jump to a known function start never extends anybody. Conditional
branches are untouched, which is what keeps Halo's `get_edge_vertex` -- the
shape `test_function_end.py` exists for -- working. Under the clamped caller the
veto fires **zero times in 6,566 walks**, so it changes only the walk that was
already wrong; on the unclamped walk it takes the maximum from 273,162 to
11,124, exactly the clamped builder's own maximum.

Cost of the defect: **45 of 1,142 interior-seed verdicts were decided by an
owner extent above 11 KB**, including `0x000556D0`, dropped as `speculative`
while being 16-aligned with three bytes of padding in front of it.

## What is still carved, and what it is

Of the 155 entries `recover_midfunction_entries.py` still translates from a mid
address to the owner's end:

| class | count | what it is |
|---|---|---|
| A | 28 | switch arm of a resynced table (defect 1 above) |
| B | **86** | an interior address of one natural compiler function |
| C | **36** | a real function start reached only by a tail `jmp` |
| F | 5 | phantom branch from a decode that ran past a `ret` into data |

Zero of the 155 has a `call` or `data_imm` xref, against a corpus of 101,669
xrefs including 29,990 calls -- so "reached by a direct call" and "address taken
as an immediate" are empty classes, and the absence is meaningful. The other
indirect-jump shapes are empty too: 214 `jmp [reg+disp32]` sites and 17
`jmp [disp32]` sites were measured the same way and none of the 155 is an entry
in any of their tables.

**C is the most actionable.** All 36 are 16-byte aligned and reached only by an
unconditional `jmp`; 30 are preceded by NOP padding or by a `ret` and padding.
`_pass_tail_jump_targets` misses them for a measured reason: **41 of the 43**
jump sites fail `if insn.address >= body_end`, because the `jmp` sits in a
region covered only by a `tail_jump_alias` fragment or a gap, and aliases do not
exist when that pass runs. One more fails because, unlike `_pass_call_targets`,
that pass never calls `engine.decode_at` on the target.

**B cannot currently be expressed.** `lifter.func_start`/`func_end` define both
the decode window and the `goto`-locality test, so a body can only `goto` a
label it also decoded. Widening the alias -- recording the owner's start beside
its end, and giving `translate_function` an `entry` distinct from `start` -- is
the route that keeps one prologue and one epilogue.

## Not measured

The downstream effect of either fix on the generated tree. Both changes are
unit-tested and the whole tool suite is green, but the regeneration and the run
that would show the switch-resolution and ABI numbers move are not in this note.
