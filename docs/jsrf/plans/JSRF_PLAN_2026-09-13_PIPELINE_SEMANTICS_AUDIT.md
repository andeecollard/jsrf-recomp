# Auditing the pipeline for more comiss-shaped bugs

13 Sep 2026. Written straight after `comiss`-on-NaN turned out to be what froze
the Corn tutorial for two days.

## Why this plan exists

The bug was not exotic. `comiss` sets ZF, PF and CF together on NaN; the lifter
emitted the unsigned conditions as the nearest C operator; C disagrees at that
one edge. It survived because **every test that uses ordinary operands passes
either way**. The same is true of every other instruction whose C translation is
"obviously" right.

So the question is not "is the lifter correct" but "where does x86 have an edge
that C does not, and do we handle it". That is a finite, enumerable question.

Ordering below is by evidence, not by pipeline order: stages 4 and 5 are where
the one confirmed bug lived and where the counters already point.

---

## Priority 0 — flag dataflow is a single address-order pass

Found 13 Sep while auditing Priority 1. This is the cause *underneath* most of
those dead branches, so it comes first.

`translator.py` builds a proper control-flow predecessor map -- and the comment
above it is emphatic about why address order is not good enough:

    # Flag state has to follow control flow, not address order: an optimising
    # compiler routinely lets a jcc consume a `cmp` from a block that is not
    # its immediate predecessor in memory.

But the *evaluation* is then a single pass in address order:

    out_state = {}
    for bb in blocks:
        ...
        elif all(p in out_state for p in sources):
            incoming = _merge_predecessor_flag_states(states)
        else:
            incoming = None          # <-- a predecessor at a higher address

So a join whose predecessor sits LATER in memory can never be resolved: its
`out_state` has not been computed when the join is reached, the `all(...)` guard
fails, and the merge is never even called. That is every loop back-edge, and
every forward jump into a shared tail.

`CSysChallengeRegionManager::calledDuringExec0Default` is exactly this shape:

    0x00015271  cmp dword ptr [ebx+0x10], 1     ; falls through
                ; XREF: 0x000152F2 (jump)
    0x00015275  jne 0x153BC                     ; <-- the join

    0x000152EE  cmp dword ptr [ebx+0x10], 2     ; the other predecessor
    0x000152F2  jmp 0x15275                     ; at a HIGHER address

Both predecessors end in a 32-bit `cmp` on the *same* operand, differing only in
the immediate. That is precisely the snapshot join
`_merge_predecessor_flag_states` was written to handle, and it is documented in
that function's own docstring. The merge never gets the chance.

**Do:** iterate the block walk to a fixed point instead of a single pass.
Compute `out_state` in a pre-pass, repeating until it stops changing (with a
small cap), then emit using the converged map. Each individual decision is
already conservative, so extra iterations can only turn `None` into a state
every predecessor agrees on -- never the reverse.

**Two cautions.** `lift_basic_block` threads `_fp_top`, the x87 stack index, so
any pre-pass must reset the per-function lifter state before each iteration or
the FPU model drifts. And a cycle of flag-transparent blocks can oscillate, so
cap the iterations and accept the last map rather than looping forever.

**Done when:** the UNRESOLVED FLAGS count drops, the tutorial still passes
(`+7930/+7934` read 1/2), and ctest shows no new failures.

**Incidentally:** `ljmp` is in `_EFLAGS_PRESERVE` and plain `jmp` is not, though
neither touches EFLAGS. Not the cause here -- an unconditional `jmp` ends a
block, so the question never arises -- but it is inconsistent and worth tidying
with this work.

## Priority 1 — the 104 branches we already know are dead

`audit_unresolved_flags.py` reports, today:

    dead fallbacks -- condition is constant zero: 104  in 57 function(s)
    jnp=15, je=15, jg=9, jne=8, loopne=7, jo=7, jp=6, jno=5, jge=5,
    jbe=4, jle=4, jl=3, jb=3, loop=3, loope=3, jae=3, ja=2, jns=2

These are branches emitted as `0`, i.e. **always not taken**, because the lifter
could not find the flag setter. This is the same failure mode as the `comiss`
bug — a conditional that silently only ever goes one way — except we already
have the list.

The ratchet is set at 76 and we are at 104, so this is also a standing test
failure. It is the single highest-value target in this document.

### Audited 13 Sep -- the 104 split three ways

**Reachability.** Only 10 of the 57 functions are called by anything at all:

    called by something :  10 functions,  18 dead branches
    no callers at all   :  47 functions,  86 dead branches

The 86 are data swept as code -- the give-away is `loop`, `loopne` and `jnp`
following `adc` at unaligned addresses like `sub_00102282`, a sequence no MSVC
output contains. They are harmless and should be excluded from the ratchet
rather than fixed, which makes the real number **18**.

**The 10 reachable ones**, two of which the decompilation names:

    sub_00130FD0  dead=5  callers=2
    sub_00056990  dead=3  callers=3   CMission::runListenerCmds
    sub_00015130  dead=2  callers=1   CSysChallengeRegionManager::calledDuringExec0Default
    sub_001237A0  dead=2  callers=3
    sub_000A0F10, sub_000B40F0, sub_000B4490, sub_000BA890,
    sub_0014B536, sub_001609E0                      dead=1 each

`CSysChallengeRegionManager::calledDuringExec0Default` is the sibling of
`CSysDeathWarpManager::calledDuringExec0Default`, which is the function that
caused the tutorial freeze, and it runs every frame.

**The mechanism, which is the actual finding.** 33 of the 104 sit immediately
after a LABEL:

    _fa = MEM32(ebx + 0x10); _fb = 1;   /* cmp MEM32(ebx+0x10), 1 */

    loc_00015275: ;
    if (_flags /* jne: ... UNRESOLVED FLAGS, branch never taken */) goto ...;

The `cmp` is the immediately preceding instruction and it sets `_fa`/`_fb`
correctly. The lifter fuses `cmp`+`jcc` only when they are ADJACENT, and a label
between them ends the basic block, so the fusion is abandoned and the condition
falls back to `_flags` -- which, as this file's own docstring says, almost
nothing ever writes. The result is a branch that is constant false.

The caution is legitimate: `loc_00015275` has one incoming `goto` and
`loc_000153A9` has seven, so the flags really do depend on which path arrived.
Emitting `0` is still the worst available answer, because it silently commits to
one path rather than admitting it does not know.

**Do:** stop treating flags as a compile-time fusion and give them a runtime
model. `_fa`/`_fb` are already function-scope and already assigned by the
comparison, so the value at the label is correct at runtime whichever path
arrived -- PROVIDED every flag setter on every incoming path writes them. Two
routes, in increasing order of work and correctness:

  1. have every flag-setting instruction write `_fa`/`_fb` plus a small kind
     tag, then emit `CMP_NE(_fa, _fb)` and friends at the branch regardless of
     adjacency; or
  2. only fuse across a label when every predecessor block ends in a flag setter
     -- a dataflow question the lifter already has the CFG to answer.

Either way, an unresolved condition must become a loud failure, not a `0`.

**Done when:** the 86 unreachable ones are excluded from the ratchet with the
reachability evidence recorded, the 18 reachable ones are resolved, and the
ratchet is lowered to match.

## Priority 2 — the instruction edge matrix

Extend `tools/recomp/test_lifter_x86_edges.py` into a systematic table: for each
implemented mnemonic, the inputs where x86 and C part company.

Known classes, with the two already fixed marked:

| edge | x86 | C / AArch64 | status |
|---|---|---|---|
| `comiss` + `je`/`jne`/`jb`/`jbe`/`jp`/`jnp` on NaN | ZF=PF=CF=1 | `==` false, `!=` true | **fixed** |
| `cvtss2si` rounding vs truncation | rounds (MXCSR) | cast truncates | **fixed** |
| float→int of NaN/inf/range | `0x80000000` | UB; ARM saturates | **fixed** |
| shift count ≥ 32 | masked to 5 bits | UB | **fixed** |
| `ROL32`/`ROR32` count 0 | no-op | shift by 32 is UB | **fixed** |
| `idiv` INT_MIN / -1, and /0 | #DE fault | UB | unaudited |
| `bsf`/`bsr` of 0 | dest undefined, ZF=1 | ? | unaudited |
| x87 80-bit intermediates | 80-bit | 64-bit double | unaudited |
| default NaN sign | `0xFFC00000` | `0x7FC00000` | known, unfixed |
| `rcpss`/`rsqrtss` | ~12-bit approx | exact | known, deliberate |
| `daa`/`aaa`/`aad`/`aas` | BCD | not implemented | see P3 |

**Do:** one test per row, asserting the emitted C, in the style of
`test_lifter_comiss.py` — each test naming what it cost or would cost.

## Priority 3 — the 176 no-op instructions

Translation emits 176 instructions as **no-op comments**, across 43 mnemonics.
A no-op comment is the most dangerous possible output: it reads as handled.

    26 out    17 bound   13 in     10 arpl   9 rcr    7 hlt
     7 outsb   7 sti      6 enter   6 insd   5 daa    4 cmpsb
     4 cmpsd   4 popal    4 popfd   3 aaa    3 aad    3 aas
     3 lcall   3 pushal   ... and 23 more

**Split these two ways before fixing anything.** Most of this list — `in`,
`out`, `bound`, `arpl`, `hlt`, `sti`, `daa`, the BCD family, `lcall` — is
implausible in an MSVC-compiled Xbox game and is far more likely to be **data
being swept as code**. That makes it a *disassembly* finding (Priority 4), not a
lifter gap, and implementing them would be wasted work.

`rcr` (9), `enter` (6), `cmpsb`/`cmpsd` (8), `popfd`/`popal`/`pushal` (11) are
plausible real code and should be implemented or explicitly refused.

**Do:** for each of the 43, check whether the address lies in a function that is
actually reached (the runtime already has `jsrf_func_hit`). Reachable + no-op is
a live bug; unreachable is a disassembly artifact.

**Done when:** every mnemonic is either implemented, or recorded as data with
the evidence, and the no-op comment is replaced by something that fails loudly
if it ever executes.

## Priority 4 — parse / disassemble / identify

These stages look healthy and should be *confirmed*, not assumed:

- **Parse.** 11 sections, 120 kernel imports, base `0x00010000`, entry
  `0x00148023` — stable across regenerations. Low risk. Check TLS and any
  section we do not map.
- **Disassemble.** Linear sweep. The P3 mnemonic census is the best available
  signal for where it walks into data. Cross-check those addresses against
  `functions.json` coverage and against the section layout.
- **Identify.** 8868 functions, 0 failed, but **1604 overlapping pairs**, of
  which 1596 interior ones are `tail_jump_alias` (shared-epilogue chains). One
  such overlap was checked by hand this session (`sub_000A000D` inside
  `sub_0009E910`) and was benign duplication. **The other 1603 have not been
  checked.** Also 163 unresolved call-target stubs and 125 recoverable
  mid-function entries.

**Do:** a script that, for every overlapping pair, asserts the outer function's
generated C contains the interior code — the property that made the one checked
case benign. Any pair failing that is a real truncation.

## Priority 5 — runtime model

- x87 stack is modelled as 8 doubles with wraparound; real x87 is 80-bit with
  tags and its own exception state. `RECOMP_FCMP` handles NaN correctly, which
  is why the bug hid in the SSE path instead.
- Default NaN sign divergence (above) — anything testing a NaN's sign bit will
  differ.
- Our heap returns dirty memory where the Xbox's was zeroed. Observed this
  session: `CPlayer +0x3CC` reads garbage once the guard correctly skips
  writing it. Harmless there; not audited generally.

## Priority 6 — compile / link

Lowest risk: 0 failed translations, the tree builds clean. Worth one pass with
`-fsanitize=undefined` on a gameplay run, which would have caught the shift and
float-cast UB directly rather than by inspection.

---

## Method note, which matters more than the list

Today's lesson was **not** "check NaN". It was that the previous handover
compared `+0xE50` against xemu, found 23 and 25 on both sides, and struck the
state band off the list — while two paths were fighting over that field every
frame. The value matched; the write rate did not.

So: when comparing against the oracle, compare **how often a field is written**,
not only what it holds. Branch and store counters at the site cost nothing
(`jsrf_func_hit` is O(1)) and would have found this in one run.

`diagnostics/jsrf_first_fault/xemu_selector_3cc.py` is the shape to copy — it
samples repeatedly, reports the distribution rather than one reading, and
carries a positive control so a dead instrument cannot be mistaken for a stable
value.

---

## Immediate, unrelated to the audit

- **Right Trigger does not register.** Tutorial is stuck at "Just get close to
  her and pull the Right Trigger". The triggers ARE mapped
  (`SDL_CONTROLLER_AXIS_TRIGGERRIGHT >> 7` into
  `bAnalogButtons[XBOX_BUTTON_RTRIGGER]`) and ARE in the pad-trace edge
  detector, but `[PAD-TRACE]` prints only `a` and `b`, so the log cannot answer
  whether a pull reaches us. Over a whole session every press was A or START
  and no trigger-only transition appeared. First step: print the trigger bytes
  in the trace line, then re-test. One line, and it makes the question
  answerable from the log.
- **Title-screen audio is wrong while in-game audio is correct.** Newly
  reported. Not investigated.
- **Title stall, ~50% of unattended runs.** Unchanged and unrelated; the clean
  binary does it too. The attract demo runs at ~54 polls/s against a healthy
  ~177, which is why a human at the pad also reads it as a dead controller.
