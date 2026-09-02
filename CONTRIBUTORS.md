# Contributors

xboxrecomp exists because people who care about the Xbox keep showing up.
Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what — the
README changelog tells the story release-by-release, but credit lives here.

Reporting a bug counts. Several of the entries below are people who never sent
a patch and still moved the project further than a patch would have, because
they found the wall everyone else was going to hit.

If you've contributed and aren't listed, or a line here is wrong, open a PR
against this file — we want every name right.

---

## Maintainer

### Ned Heller — [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer. The x86 disassembler and lifter, XBE parsing,
the kernel/XAPI runtime and HLE layer, the D3D8 and NV2A translation, the XISO
and XMV tooling, and the recompilation pipeline that ties them together.

---

## Contributors

### NoRain211 — [@NoRain211](https://github.com/NoRain211)
A correctness batch across the disassembler, lifter and translator, found by
stress-testing the pipeline against a real title. Almost all of it is the
dangerous kind of bug: the generated C compiles, links, runs, and is quietly
wrong, with no lifter error or warning anywhere.

*Control-flow recovery (#7)*
- **Conditional tail calls skipped the frame bridge** — `jcc` to a known
  function entry is a tail call, but only the unconditional form emitted the
  bridge, so the taken edge jumped into the callee with the caller's frame
  still live. 8,263 call sites across 5,426 functions on the title tested.
- **Fall-through off the end of a function emitted nothing** — a block running
  off its own end into the next function produced no transfer at all; control
  fell out of the generated switch and returned. 3,183 sites.
- **Indirect calls read the target after the return-address push** — `call
  [esp+X]` computed its operand once `esp` had already moved, so the target
  came from the wrong slot. The operand is now snapshotted before the push.
- **Computed jump targets were not owned by the enclosing function**, and
  `--seed-functions` can now be repeated to merge several seed sets.

*Flag computation (#8)*
- **`repe cmpsb` / `repne scasb` folded their result flags to a literal 1**, so
  every `memcmp`/`strcmp`-shaped loop in the CRT reported "equal" regardless of
  input. ZF/CF now derive from the last pair actually compared.
- **`NEG` carry was lost before a dependent `SBB`/`ADC`** — CF was preserved
  only when the consumer was the very next instruction, but real codegen
  separates them with flag-safe instructions, and the borrow silently went to
  zero. That is the standard 64-bit subtract and sign-extend idiom, so the
  corruption lands squarely in integer math.
- **Signed compares were evaluated at 32 bits regardless of operand width**, so
  `cmp al, bl` + `jl` never had the sign bit in the right place.

*x87 and SSE (#9, #10)*
- **Packed SSE was lifted as a scalar `float`** — XMM was modelled as a single
  float, so `movaps`/`movups` transferred 4 of 16 bytes and silently discarded
  the upper three lanes (18,439 moves), and packed arithmetic had no pattern at
  all: 561 operations dropped outright.
- **904 x87 instructions across 28 mnemonics lifted to comments**, desynchro-
  nising the FPU stack from that point on. The reverse forms are the nastiest —
  `fdivr` against 1.0 is a reciprocal, and dropping it turns a vector normalise
  `v/len` into `v*len`.
- **`FNSTCW` / `FNSTSW` were comments only**, so `_control87` could not read
  back rounding or precision mode and every `fcom`-derived parity test read a
  hardcoded `true` (1,326 sites). Both words are now modelled, and x87 state is
  shared rather than reset per function.
- **XMM was declared as a C local in each generated function**, so a value
  written in one lifted block and read in the next was lost to a fresh zeroed
  local. It is guest state now.

*(The runtime half of the SSE work — the `RecompXmm` union and the 28 `XMM_*`
helpers the lift emits — was not in the PRs and was added on integration.)*

*Missing instructions and dispatch (#14, #15, #16)*
- **`XLAT`/`XLATB` lifted to nothing**, so a guest table lookup left `AL`
  unchanged. Now `AL = MEM8(EBX + zero_extend(AL))`, written through `SET_LO8`
  so only the low byte moves, with the `0x67` address-size override using `BX`
  and wrapping the offset at 16 bits.
- **`BSF`/`BSR` were classified as flag setters but never lifted**, leaving an
  unhandled-instruction comment where the result should be. The flag half is
  the subtler fix: ZF was derived by re-reading the source operand at the
  branch, so any flag-preserving write between the scan and the `jcc` silently
  changed the answer. ZF is snapshotted where the scan runs, and the translator
  omits the snapshot when nothing consumes it. A zero source leaves the
  destination untouched, matching the architectural "undefined" instead of
  inventing a value.
- **`recomp_lookup_manual` was consulted on indirect calls only** — a direct
  call or tail jump went straight to the generated body, so a hand-written
  replacement worked through a function pointer and was quietly bypassed by
  every direct caller. Now direct calls and both tail-jump forms route through
  the manual set that `--manual-functions` and `--exclude-manual` already build.

*Also raised: stored code pointers (#13).* The gap is real and was found
independently while bringing up Half-Life 2 -- functions reachable only as an
address in a table have no call site, no prologue and no padding boundary, so
nothing else finds them. It is now covered by `_pass_imm_ref_targets` and
`_pass_data_ptr_targets`, which reached the same conclusion from the other
direction.

### DarthSidious666 — [@DarthSidious666](https://github.com/DarthSidious666)
- **Implemented the missing `tools/abi_analysis` (#6)** — the pipeline had a
  hole in it: `tools.recomp` looked for `abi_functions.json`, warned when it
  was absent, and then fell back to `cdecl` / 0 params / `int_or_void` for
  every single function, because the tool that was supposed to produce that
  file did not exist. Recovers calling convention (including thiscall from
  ecx-read-before-write), parameter count from the `ret` immediate, return-type
  hints and frame shape, so the generated signatures are real.
- Also **diagnosed the tail of issue #2**, narrowing it from "recomp crashes"
  to the specific missing tool, and posted a workaround before the PR.

---

## Issue reports and testing

### Tiptup300 — [@Tiptup300](https://github.com/Tiptup300)
- **Found that every documented step of the getting-started guide was broken
  (#1)** — and found it on Linux, which is not the platform any of it had been
  tried on. The report walked the whole pipeline: `tools.xbe_parser` not being
  runnable as a module, step 2 never emitting the JSON that step 3 requires,
  the ABI file that does not exist, and the crash at the end of `tools.recomp`.
  That single issue is the origin of the pipeline fix in `21488f4` — and of the
  repository having a LICENSE file at all, which the README had claimed for
  months without one actually existing.

### M0RSM4LLEO — [@M0RSM4LLEO](https://github.com/M0RSM4LLEO)
- **Reproduced and pinned down the getting-started failures (#2)** with the
  kind of detail that makes a report actionable: exact commands, exact
  tracebacks, tool versions, and the observation that `tools/xbe_parser` was
  the only tool package with no `__main__.py` while every other one had it.
  Kept testing through each fix and reported what broke next, which is how the
  `write_summary` crash at the very end of a full run got found.

---

## A note on AI-assisted contributions

Parts of this project — and some contributions to it — were developed with the
help of AI coding tools. That's welcome here: what matters is that every change
is understood, reviewed, and verified by a human before it lands. If you used
an assistant, just say so in your PR (several contributors have) and make sure
you can stand behind the result.
