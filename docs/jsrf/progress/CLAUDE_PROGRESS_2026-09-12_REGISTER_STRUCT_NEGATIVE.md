# The register-context experiment: measurable codegen change, no speedup

Date: 2026-09-12 (Europe/London), late
Result: **NEGATIVE. Reverted. Do not repeat it.**

## What was tried

The profile said `_tlv_get_addr` was ~75% of the main guest thread's samples --
more than the rasteriser, the combiner and the ADX decoder together. The
generated code is `#define eax g_eax` over eight separate `RECOMP_TLS uint32_t`
scalars, and Darwin has no ELF TLS models, so each access is a tlv descriptor
load plus an indirect call into libdyld.

A compiler can only fold repeated accesses to the SAME thread-local. So the
eight scalars were consolidated into one:

```c
typedef struct RecompGpr { uint32_t r_eax, r_ecx, ..., r_esp; } RecompGpr;
extern RECOMP_TLS RecompGpr g_gpr;
#define g_eax g_gpr.r_eax
```

Every existing name keeps working, in the runtime and in the 1.3M lines of
generated code alike, so **no generated code had to be edited**.

Micro-case, confirmed before doing the real thing: a function touching eight
thread-local scalars emits eight indirect calls; the same function over one
thread-local struct emits **one**.

## It worked, and it did nothing

The change took effect and is not in doubt. `recomp_0000.c.o`, the hottest
generated object:

    before   8,322,960 bytes
    after    7,254,880 bytes      -- 13% less code

The runtime did not move at all:

    title crossover     baseline t=30.55s     struct t=30.69s
    gameplay loop rate  ~19 iterations per 2s sample in BOTH

That is noise, not a win.

## Why the profile lied

`sample` attributes a stack sample to whatever the PC is in. `_tlv_get_addr` is
a tiny leaf called constantly, so the PC lands there very often -- which makes
it the top entry without making it the top *cost*.

The likely mechanism for the rest: clang cannot cache a TLS address across a
call, because the descriptor function is not `readnone`. Generated functions are
full of `RECOMP_ABI_CALL`, so consolidating helps only within straight-line
stretches between calls, and those are short.

## The lesson, which is the point of writing this up

**A profiler's top entry is a hypothesis, not a cost.** The only thing that
settles it is removing the work and measuring again. This cost one experiment
and would have cost a week had it gone straight to the translator change --
which is what "hand each generated function a register-context pointer" would
have required, for the same non-result.

## Do not conclude

...that guest register access is cheap, or that TLS is fine. Neither is shown.
What is shown is that going from eight lookups to one, in the hot path, buys
nothing measurable on this host. A design that removes the lookup *entirely*
might still pay -- but it now has to justify itself against this result first,
and it should be prototyped on a handful of functions and measured before
anything touches the translator.

## Still open, in order

1. The GPU rasteriser: Windows has none (the accelerated path is
   `#ifdef __APPLE__`), and macOS is also slow. See
   `CLAUDE_HANDOVER_2026-09-12_TLS_IS_THE_BOTTLENECK.txt` section 4.
2. The clang/native-TLS Windows build is a measured 2.2x and is already in
   `tools/windows/clang-mingw.cmake` -- that one was real.
