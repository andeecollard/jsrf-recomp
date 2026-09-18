# The Windows cross-build, and the portability bug it found — 18 Sep 2026

Prompted by a Steam Deck question. The Deck is x86-64 Linux; Proton runs
Windows binaries on it with DXVK translating D3D11 to Vulkan. Since this tree
already has a D3D11 NV2A backend and a committed mingw toolchain file, the
cheapest test of the whole idea was to cross-build and see how far it got.

It got to the linker, and on the way it found a bug that affects **every
non-ARM64 host**, not just Windows.

## The bug

    mcpx_hw_store             defined ONLY inside
    mcpx_hw_store_n                #if !defined(_WIN32) && defined(__aarch64__)
    mcpx_hw_store_n_or_last

    xbox_memory_layout.c:598    calls mcpx_hw_store_n          guard: NONE
    xbox_memory_layout.c:1249   calls mcpx_hw_store_n_or_last  guard: NONE

The whole MCPX guarded-write helper family is Apple-Silicon-only and every
caller is unconditional. The guard exists because the block it sits in is the
signal-handler MMIO trap — `#include <signal.h>`, `<ucontext.h>`, and ARM64
store-instruction decoding — but the *store helpers* were put inside it along
with the fault handler.

**So this is not a Windows problem.** A native x86-64 Linux build — the actual
Steam Deck target — fails identically. It has been invisible because nothing
but macOS ARM64 has been linked since it landed. The linker reports only
`mcpx_hw_store_n_or_last` because it stops there; `:598` is the same bug one
symbol along.

## Why no fix is offered here

It looks tractable: `g_mcpx_alias` (line 338) and `MCPX_WBASE` (line 400) are
both at file scope *outside* the guard, so a portable definition could write
through the alias without any of the ARM64 fault-handler machinery.

The reason not to write it blind is at line 326:

> The comment on `mcpx_hw_store_n` concluded that windows "cannot be eliminated
> while the mechanism is mprotect, so the rule is to open as few as possible".
> This removes that constraint rather than living within it: the aperture is
> backed by a file mapping and mapped twice, so the runtime writes through an
> alias that is always writable while the guest's view is never unprotected.

That double mapping is the fix for the incident CLAUDE.md records as **13.5
million open windows in 45 seconds, swallowing every `VOICE_ON`**. Windows
traps MMIO with a vectored exception handler rather than `mprotect`, so whether
a non-ARM64 definition needs the same locking and window discipline is a
question about this subsystem, not a build fix. Guessing at it means writing
MMIO code for the exact hazard this project has already been burned by.

**It needs a decision from someone who owns that path**, and the decision is
cheap to state once made: either give the helpers a portable definition that
writes through `MCPX_WBASE`, or guard the two call sites and give OHCI a
different write path off ARM64.

## The two shims that are fixed

Both match patterns already in the tree rather than inventing anything:

- **`sched_yield`** (`kernel_bridge.c`, 2 call sites). mingw has no
  `<sched.h>`. `SwitchToThread` is the Windows equivalent and is already what
  `xbox_memory_layout.c`'s aperture waits and `apu_core.c`'s `APU_LOCK_YIELD`
  spin on. Behind a `recomp_yield()` macro in the existing platform guard.
- **`clock_gettime` / `CLOCK_MONOTONIC`** (`main.c`, 1 call site, inside a
  diagnostic gated on `RECOMP_ADX_RATE`). mingw declares neither.
  `QueryPerformanceCounter` is the Windows monotonic clock and
  `apu_shim.h`'s `qemu_clock_get_us` already uses exactly those two calls.

macOS after both: **53/53, clean build.** Neither shim changes the native path.

## Where the cross-build actually stands

    configure                     clean; "gen tree matches tools/recomp"
    xbox_nv2a / vsh / input /
      apu / video / kernel        compile clean for x86-64 Windows
    jsrf_first_fault.exe          [80%] Linking -- 1 undefined reference
    4 test executables            linked as real PEs

So: the engine compiles in full for x86-64 Windows, including everything
changed today, and stops at one symbol that is a genuine subsystem decision.

## What this says about the Deck idea

Better than it looked this morning.

- **Proton route.** Needs the Windows binary. One decision away. Under Proton,
  Wine supplies `d3dcompiler_47` for the runtime HLSL that
  `nv2a_d3d11.c:486` needs, and DXVK supplies D3D11 — so the binary should run
  unmodified.
- **Native Linux / dxvk-native.** Also needs a `D3DCompile` replacement,
  because DXVK implements D3D9/10/11 but not the HLSL compiler. Pre-compiling
  our own generated shaders at build time is the tractable version of that.
- **Either way, the MCPX bug blocks both**, because it is not Windows-specific.
  Fixing it is the single highest-value portability change available.

x86-64 is also architecturally kinder than the current host: a whole class of
lifter pain exists because AArch64 saturates float-to-int where x86 stores the
integer indefinite (`RECOMP_F2I_TRUNC`, `RECOMP_INT_INDEFINITE`).
