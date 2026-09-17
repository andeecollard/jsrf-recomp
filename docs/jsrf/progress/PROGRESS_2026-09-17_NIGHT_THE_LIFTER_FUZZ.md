# Fuzzing our own lifter, and what fell out — 17 September 2026, night

Upstream's PR #62 adds a deterministic differential fuzzer for the x86 lifter
and reports **205 mismatches across 4,000 vectors** against their tree. We
pointed the same idea at ours. It found three defect families in the first
hour, one of which is **live in the player's build**.

## Why the oracle is Unicorn, and what that costs

Upstream's oracle is **native 32-bit x86 execution**, which this host cannot
provide by any route: Apple Silicon with no Rosetta installed — and Rosetta 2
cannot execute 32-bit x86 even when it is — no Docker, and the conformance
runner shells out to `cl.exe` with MSVC inline-asm syntax. dplewis's PR #59
exists precisely because of this, and solves it with two Docker images.

`tools/conformance/fuzz_unicorn.py` runs entirely locally instead:

    Intel-syntax text -> clang(--target=i386) -> bytes
    bytes -> Unicorn x86-32 core                   -> eax     (the oracle)
    bytes -> our disasm + lifter -> C -> clang     -> eax     (what we ship)

**Unicorn is a second model, not silicon.** A disagreement is a lead, not a
verdict. The first run proved the point on itself: 9 of 24 "mismatches" were a
`setcc` reading a flag that BT/BTS/BTR/BTC leave *architecturally undefined*
(SDM Vol 2A defines CF only). Those compared two models' choices of undefined
and nothing else. The generator no longer emits them.

Run: `seed=0x7 generated=600 ran=542 skipped=58 vectors=13008` →
**860 mismatches across 60 cases** — `bit=34, shift=20, cmov=3, incdec=2,
cmpset=1`. All 58 skips are cases where the lifter emitted a bare comment; the
fuzzer skips those rather than scoring them, so they are a coverage loss and
not a false positive.

---

## L1 — The SF condition is signed-overflow UB *(CORRECTED: not reachable here)*

> **Correction, later the same night.** The paragraph below said "this one is
> live" and gave five sites. The defect and the `-O` demonstration are right;
> the reachability claim was not. JSRF has **701** SF consumers, not five —
> five was the count of one *expression form*. 252 are `TEST_S` (an AND, which
> cannot overflow), 317 more are AND-based, ~127 take the sign of a single
> value, and the five UB-form sites **all read `cmp <mem>, 0`**, where
> `_fas - 0` cannot overflow. So the fold is a no-op at every site in this
> image. I counted a grep and never read the operands. Details in the goals
> file's G14. The rest of this section stands as written.

**This one is live.** The lifter emits, for `js`/`sets`/`cmovs` after a `cmp`:

    if (((int32_t)((_fas) - (_fbs)) < 0)) goto loc_000A9E04;   /* js */

x86's SF is bit 31 of the **wrapped** difference. This C is a signed
subtraction in `int32_t`, and signed overflow is undefined behaviour, so the
compiler is entitled to fold `(a - b) < 0` into `a < b` — which is a different
function whenever the subtraction overflows.

**Measured, not argued.** The exact expression, with `_fas = 0x80000000` and
`_fbs = 1`, where x86 gives SF=0 because `0x80000000 - 1 = 0x7FFFFFFF`:

    -O0  SF-condition taken = 0      <- correct
    -O1  SF-condition taken = 1      <- wrong
    -O2  SF-condition taken = 1      <- wrong

`build-feav` is `CMAKE_BUILD_TYPE=RelWithDebInfo` with
`JSRF_OPT_LEVEL=-O2`. **The player's build takes the wrong branch.**

**Five sites in JSRF's generated C**, three distinct, all `js`:

    recomp_0002.c:95857, :95935          sub_000A9830
    recomp_0008.c:9199                   sub_00192830
    recomp_stubs_unresolved.c:40149,:40227   sub_000A9851 (same code, second gen)

Whether those three `js` sites ever see operands that overflow is a **runtime**
question and is not yet answered. The code is wrong; the reachability is
unmeasured. Do not skip that step.

**The methodological sting, which is worse than the bug.** This defect is
*optimisation-level dependent*. Any A/B or bisect that compared an `-O0` tree
against an `-O2` tree was comparing two different semantics, not two different
changes. `diagnostics/jsrf_first_fault/CMakeLists.txt` records that the
bring-up build was `-O0` and that `JSRF_OPT_LEVEL=-O0` still restores it.

**Fix:** compute SF the way the hardware does — the sign bit of the wrapped
result, in unsigned arithmetic: `((uint32_t)_fa - (uint32_t)_fb) >> 31`. Note
that `jl`/`jge`/`jle`/`jg` are **not** affected: SF≠OF is mathematically signed
less-than, so emitting `_fas < _fbs` for those is correct. The bug is specific
to conditions that read SF or OF alone — `s`, `ns`, `o`, `no`.

**Done when:** the fuzzer's `cmpset`/`cmov`/`incdec` families are clean, a
regression test pins the `0x80000000 / 1` vector at `-O2`, and the three `js`
sites have been counted at runtime.

## L2 — `bts`/`btr`/`btc` report CF *after* their own write

    eax = (eax | (1u << (ecx & 31)));                      /* bts */
    SET_LO8(edx, (((eax >> (ecx & 31)) & 1)) ? 1 : 0);     /* setb */

CF is reconstructed at the consumer by re-reading the bit out of the register
(`lifter.py:872`) — but `bts`/`btr`/`btc` have already modified that bit. So
CF reports the **new** value, and after `bts` it is always 1, after `btr`
always 0, regardless of what was there.

That is the test-and-set idiom — "was this flag already set?" — reading its own
answer. Plain `bt` is fine, because it modifies nothing.

**Latent for JSRF: zero `bts`/`btr`/`btc` in the generated image.** Real for
the recompiler and for any other title, so this is upstream's to have.

**Done when:** the bit's pre-state is snapshotted at the instruction rather
than reconstructed at the consumer.

## L3 — Narrow rotates rotate at 32 bits

    SET_LO8(eax, ROR32(LO8(eax), 2));         /* ror al, 2 */

`LO8` zero-extends, so `ROR32` rotates the byte inside a 32-bit word and the
bits that should wrap around at bit 7 fall into bits 31..8, which `SET_LO8`
then discards. `ror al, 2` with `al=0x01` gives `0x40` on x86 and `0x00` here.
`rol ax, 7` with `ax=0xFFFF` gives `0xFFFF` on x86 and `0xFF80` here.

The count is also not reduced modulo the operand width: x86 masks to 5 bits and
then rotates within 8 or 16, so `rol ax, 31` is a rotate by 15, and `rol al, 16`
is a rotate by 0 — an identity we currently turn into zero.

**This is the same defect class as the `sar` width bug** already fixed in this
tree (`test_lifter_sar_width.py`) and contributed upstream in PR #57: a narrow
operand evaluated at 32 bits. The rotates were missed when `sar` was fixed.

**Latent for JSRF: zero narrow rotates in the generated image.** All 13
`ROL32`/`ROR32` uses are full width. Upstream's to have.

**Done when:** `ROL8/ROR8/ROL16/ROR16` exist, with the count taken `mod` width.

---

## What this does not cover

GPR integer only. No memory operands, no FPU, no SSE, no faulting
instructions, and the comparison is **EAX after the snippet** — not the flags
themselves, so a wrong flag is only caught when a `setcc` or `cmov` in the same
snippet spends it. Extending to an FPU oracle would also put a number on G13.

## Ordering

None of this outranks G3/Track A, and L2 and L3 do not affect the player at
all. **L1 does**, and it is a handful of lines with a control that already
exists — but it changes generated code for every `js` in the image, so it wants
a regenerated gen and a re-verified baseline, which is exactly the cost
CLAUDE.md warns about. Do it deliberately, not in the middle of an
investigation that is quoting numbers.
