/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest debug output: INT 2D, the Xbox kernel debug trap.
 *
 * eax selects the service, ecx carries its argument. Service 1 prints the
 * ANSI_STRING ecx points at -- what OutputDebugStringA compiles to. The int3
 * that always follows is the slide byte the kernel steps over, so the lifter
 * emits nothing for it; lifting it to __debugbreak() terminated the process
 * with STATUS_BREAKPOINT the first time a title tried to print. */
void recomp_debug_service(uint32_t service, uint32_t arg_va);

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions. */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* Bounds of the title's executable sections, filled in by
 * xbox_MemoryLayoutInit from the XBE's own section table. Zero means "not
 * loaded yet", which RECOMP_ICALL_IS_CODE treats as allow.
 *
 * These replace a hardcoded 0x00400000 cutoff that was only ever right for one
 * title -- see the comment where they are defined in xbox_memory_layout.c. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* Is this indirect-call target plausibly code?
 *
 * Synthetic kernel thunks live at 0xFE0000xx and are dispatched by
 * recomp_lookup_kernel, so they are always allowed. Everything else has to lie
 * inside an executable section of the loaded title. A target that fails this is
 * a garbage pointer that reached a call site, and calling it would be worse
 * than dropping it. */
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Per-thread register state.
 *
 * These started as plain globals, which works exactly as long as one thread
 * runs recompiled code. Halo is the first title to create real workers (its
 * cache/file loader), and the runtime papered over that by running every worker
 * synchronously inside PsCreateSystemThreadEx -- so a worker that blocks
 * waiting for work never returns and startup deadlocks.
 *
 * On hardware each thread has its own register set, so model it that way.
 * Thread-local costs an indirection per access; a deadlock costs the title. */
/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise.
 *
 * MEASURED 12 Sep 2026, and the biggest single cost in the program on BOTH
 * hosts. The generated code is `#define eax g_eax` over this register set, so
 * every guest register read and write is one access to a thread-local.
 *
 *   macOS   Darwin has no ELF TLS models, so __thread always goes through the
 *           tlv descriptor and _tlv_get_addr in libdyld, and -ftls-model= is
 *           silently a no-op. `sample` puts _tlv_get_addr at roughly 75% of
 *           the main guest thread's samples -- more than the rasteriser, the
 *           combiner and the ADX decoder put together.
 *   Windows mingw-w64 defines __GNUC__, so it takes the __thread branch, and
 *           GCC implements __thread on this target with EMULATED TLS:
 *           __emutls_get_address(), a real call with a lookup inside it, per
 *           access. jsrf_first_fault.exe carries 145 emutls symbols.
 *
 * TRIED AND REVERTED: keying the choice on _WIN32 so mingw takes
 * __declspec(thread) (native TEB-relative, no call). It builds, and emutls
 * symbols drop 145 -> 16, but the title exits through HalReturnToFirmware
 * within 16 file opens. Native PE TLS needs its directory and per-thread
 * initialisation, and something in the harness's own thread creation or memory
 * layout does not satisfy that. Do not simply re-apply it; find out why first.
 *
 * The real fix is not a storage class. It is to stop doing a TLS lookup per
 * register access -- hand the generated functions a register-context pointer
 * once on entry and index through it. That needs a translator change or a
 * mechanical edit of the generated tree, and it is worth doing: it is the
 * single largest measured win available on either host.
 *
 * RECOMP_TLS may be predefined by the build to override the choice below;
 * -DRECOMP_TLS= builds with plain globals, which is not thread-safe and
 * crashes, but is useful for bounding experiments. */
#ifndef RECOMP_TLS
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

/* RDTSC. Defined in the runtime (src/kernel/recomp_tsc.c) rather than inline,
 * because a title reads the counter as a clock and every caller has to agree
 * on its rate: the guest's own QueryPerformanceFrequency reports 733333333. */
uint64_t recomp_rdtsc(void);
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* Guest linear base loaded in the x86 FS segment register. Xbox uses FS for
 * per-thread state, so flattening an fs:[offset] operand to MEM(offset) makes
 * all guest threads share (and corrupt) low memory. */
extern RECOMP_TLS uint32_t g_fs_base;

/* x87 stack. Per-thread for the same reason the integer registers are:
 * arguments are passed in st(0)/st(1) across call boundaries. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
/* Linear base of the fs segment: where the fake TIB lives. Generated code
 * adds this to every fs-relative address, so page zero stays unmapped and a
 * null dereference faults instead of hitting the TIB. Must match
 * XBOX_FS_BASE in src/kernel/xbox_memory_layout.h.
 *
 * Per-thread, because a TIB is. fs:[0] is the SEH chain head and fs:[4]
 * reaches the CRT's per-thread data, so a single shared base makes every
 * guest thread the same thread as far as the CRT is concerned. */
extern RECOMP_TLS uint32_t g_fs_base;

/* The #undef is deliberate and the comment above understates the situation:
 * these are not the same thing, and "must match" is not what happens.
 *
 * xbox_memory_layout.h defines XBOX_FS_BASE as the CONSTANT 0x00001000. This
 * file defines it as the per-thread VARIABLE. Which one a translation unit
 * gets is decided purely by include order, and today that happens to give
 * every TU the one it needs:
 *
 *   runtime C (xbox_memory_layout.c, kernel_bridge.c) never includes this
 *       header, so it gets 0x1000 -- correct, because it uses XBOX_FS_BASE to
 *       initialise the PRIMARY TIB and as the "page zero is unmapped" bound
 *       in the mapped-address check
 *   generated code includes both, so it gets g_fs_base -- correct, because
 *       fs:[0] is the SEH chain head and fs:[4] the CRT's per-thread data,
 *       and a shared base makes every guest thread the same thread
 *
 * That is luck resting on an include list, not a design, and it was silent for
 * as long as the title was built with a blanket -w. Give kernel_bridge.c a
 * reason to include this header and its null-pointer check quietly becomes
 * "below THIS THREAD's TIB", which for a spawned thread rejects a large range
 * of perfectly valid low addresses.
 *
 * #undef preserves exactly today's behaviour -- this definition still wins
 * wherever both are visible -- while making the override deliberate and
 * stopping it hiding behind a warning nobody reads. Do NOT "fix" this with
 * #ifndef: that flips generated code onto the constant and gives every guest
 * thread the primary thread's TIB. */
#undef XBOX_FS_BASE
#define XBOX_FS_BASE g_fs_base

extern RECOMP_TLS uint32_t g_seh_ebp;

/* ---- non-local jumps (setjmp / longjmp) --------------------------------
 *
 * A recompiled function is a real C function, so restoring the guest esp is
 * only half of a longjmp: the abandoned frames are still on the native stack,
 * and control returns into them once the resume point finishes. Pairing each
 * guest jmp_buf with a native one -- taken at the setjmp call site, the only
 * place a native setjmp is valid -- makes the guest jump a native jump, so the
 * frames actually unwind.
 *
 * recomp_setjmp_slot() returns the native buffer for a guest jmp_buf address,
 * allocating one on first use. recomp_guest_longjmp() restores the guest
 * registers the CRT's longjmp would and jumps; it returns 0, without jumping,
 * for a buffer that has no native counterpart, leaving the caller to fall back
 * to the guest's own longjmp.
 */
/* Atomic read-modify-write, for the lock-prefixed instructions.
 *
 * "lock xadd" and "lock cmpxchg" are what InterlockedIncrement and
 * InterlockedCompareExchange compile to, so they carry a title's reference
 * counts and its lock-free lists. They used to lift to a TODO comment, which
 * meant a refcount that never moved and a compare-and-swap that never swapped
 * -- harmless while every guest thread ran synchronously, and not once the
 * runtime began spawning real ones.
 *
 * Genuinely atomic, not merely correct in isolation: the guest's own threads
 * now run on real host threads, so a read-modify-write that races is exactly
 * the bug these instructions exist to prevent.
 */
#if defined(_MSC_VER)
#include <intrin.h>
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)_InterlockedCompareExchange((volatile long *)(p),                                            (long)(val), (long)(cmp)))
#else
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)__sync_fetch_and_add((volatile uint32_t *)(p), (uint32_t)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)__sync_val_compare_and_swap((volatile uint32_t *)(p),                                            (uint32_t)(cmp), (uint32_t)(val)))
#endif

#include <setjmp.h>
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
extern RECOMP_TLS uint32_t g_ebp;

/* EFLAGS.DF, and the signed step the string instructions take because of it.
 *
 * `cld`/`std` used to lift to a comment and every string instruction stepped
 * forwards regardless. That is right almost everywhere -- DF is 0 by ABI and
 * the compiler restores it -- but MSVC's strrchr is `std; repne scasb` from
 * the terminator backwards, so it scanned forwards off the end of the string
 * and returned a pointer into whatever followed. The Xbox Dashboard uses it to
 * split "y:\default.xip" into a mount path; with strrchr wrong the archive
 * registered itself under its own full filename, no resource in it could ever
 * be found by name, and the dashboard rebooted rather than showing a UI.
 * memmove's overlapping case is the same instruction and the same bug.
 */
extern RECOMP_TLS int g_df;
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

/* x87 control and status. Thread-local for the same reason the x87 stack
   above is: one guest routine can lift to several C functions, so a compare
   and the FNSTSW that reads it can land in different bodies, and the control
   word has to survive a call. (g_fp_stack/g_fp_top are declared above.) */
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_cc;
#define RECOMP_FCMP_CC(c) ((uint16_t)((c)==2 ? 0x4500u : (c)<0 ? 0x0100u : (c)>0 ? 0u : 0x4000u))
/* Values in the existing double-backed stack are all representable as normal
 * x87 extended values, including binary64 subnormals. Empty stack tags and
 * unsupported extended encodings are not represented by this stack model. */
static inline uint16_t recomp_fxam(double value) {
    return (uint16_t)((signbit(value) ? 0x0200u : 0u) |
        (isnan(value) ? 0x0100u : isinf(value) ? 0x0500u :
         value == 0.0 ? 0x4000u : 0x0400u));
}

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
/* MXCSR as the guest will find it.
 *
 * 0x1F80 is the architectural reset value: all six exception flags clear, all
 * six masks set, rounding control 00 (nearest-even), flush-to-zero and
 * denormals-are-zero off. JSRF reads it exactly once, in sub_0015F0F0, which
 * does `stmxcsr` then `test ah, 0x60` -- bits 13-14, the rounding-control
 * field -- and returns 0x8000FFFF if they are not zero. `ldmxcsr` never appears
 * anywhere in the title, so the guest never changes it and a constant is the
 * whole of the model.
 *
 * While stmxcsr was emitted as a no-op comment that check read whatever stale
 * bytes happened to be at 0x265150, so it passed or failed by luck. */
#define RECOMP_MXCSR_DEFAULT 0x1F80u

#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)
/* x87 integer stores use the guest RC bits, independently of host rounding.
 * Masked invalid conversions store the signed integer-indefinite value. */
static inline int64_t recomp_fist(double value, uint16_t control, unsigned bits) {
    double rounded;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(rounded) || rounded < -limit || rounded >= limit)
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}

/* x86 float -> int32, which a plain C cast does NOT reproduce.
 *
 * On x86 a source that is NaN, infinite, or outside int32 range yields the
 * "integer indefinite" 0x80000000. In C the same cast is UNDEFINED BEHAVIOUR,
 * and AArch64 saturates instead -- NaN gives 0, +inf gives INT_MAX -- so the
 * two hosts disagree exactly where the guest is most likely to be relying on
 * the sentinel.
 *
 * The two forms differ in rounding, which is the part that is wrong for
 * ordinary values and not just at the edges: CVTTSS2SI truncates toward zero,
 * CVTSS2SI rounds under MXCSR, whose default is nearest-even. Emitting both as
 * `(int32_t)x` silently turns every CVTSS2SI into a truncation.
 *
 * The range test is written as a negated AND so that NaN, which compares false
 * against everything, falls into the indefinite case rather than past it. */
#define RECOMP_INT_INDEFINITE ((int32_t)0x80000000)

static inline int32_t RECOMP_F2I_TRUNC(double v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return RECOMP_INT_INDEFINITE;
    return (int32_t)v;
}

static inline int32_t RECOMP_F2I_ROUND(double v) {
    double r;
    if (v != v) return RECOMP_INT_INDEFINITE;   /* nearbyint would propagate it */
    r = nearbyint(v);                           /* current mode; default nearest-even */
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return RECOMP_INT_INDEFINITE;
    return (int32_t)r;
}

/* The other two destination widths, for FIST/FISTP.
 *
 * The indefinite is the destination's own most-negative value, not the int32
 * one: a `fistp word ptr` out of range stores 0x8000, and a `fistp qword ptr`
 * stores 0x8000000000000000. Narrowing the int32 helper's answer would write
 * 0 into a 16-bit slot and lose the sentinel entirely.
 *
 * Only the rounding form exists at these widths because only FIST/FISTP
 * reaches them, and FIST always rounds -- under the x87 control word rather
 * than MXCSR, but with the same nearest-even default, which is what nearbyint
 * gives under the host's untouched rounding mode. There is no 16- or 64-bit
 * CVTT to want a truncating sibling.
 *
 * The int64 bound has to be written as the literal 2^63 and not as
 * (double)INT64_MAX: INT64_MAX is not representable as a double and converting
 * it rounds UP to 2^63, so `r < (double)INT64_MAX` would admit exactly the one
 * value that overflows. 9223372036854775808.0 is a power of two, so it is
 * exact, and as an exclusive bound it is the correct one. The low bound is
 * inclusive for the same reason in reverse -- -2^63 IS int64's minimum. */
#define RECOMP_INT16_INDEFINITE ((int16_t)0x8000)
#define RECOMP_INT64_INDEFINITE ((int64_t)(-9223372036854775807LL - 1))

static inline int16_t RECOMP_F2I16_ROUND(double v) {
    double r;
    if (v != v) return RECOMP_INT16_INDEFINITE; /* nearbyint would propagate it */
    r = nearbyint(v);
    if (!(r >= -32768.0 && r < 32768.0)) return RECOMP_INT16_INDEFINITE;
    return (int16_t)r;
}

static inline int64_t RECOMP_F2I64_ROUND(double v) {
    double r;
    if (v != v) return RECOMP_INT64_INDEFINITE; /* nearbyint would propagate it */
    r = nearbyint(v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return RECOMP_INT64_INDEFINITE;
    return (int64_t)r;
}

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);

/* Report an indirect call whose target is not code (a null or wild
 * function pointer). Rate-limited per address by the implementation,
 * because these usually arrive inside a loop -- which is exactly why
 * they must be reported: silently skipping one turns a diagnosable null
 * vtable call into an unexplained hang. */
void recomp_icall_not_code_log(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
/* The guest's time source.
 *
 * Xbox's QueryPerformanceCounter is a bare rdtsc and its
 * QueryPerformanceFrequency returns the CPU clock as a constant, so the guest
 * divides this by 733,333,333 to get seconds. Returning the host TSC would
 * make that division wrong by the ratio of the two clocks; the runtime scales
 * to the console's rate instead. */
uint64_t xbox_ReadTimeStampCounter(void);

/* Guarded because a harness may force-include its own header that defines
 * these first -- JSRF's guest_trace.h is force-included via -include and does
 * exactly that. Unguarded, the two collide once per translation unit, and the
 * collision was invisible for as long as the title was compiled with a blanket
 * -w. It happens to be harmless today: the two definitions differ only by a
 * (uint32_t) cast on va, and this one wins because it is textually later. That
 * is luck, not design -- whoever next gives a harness a DIFFERENT tracer would
 * find this one silently overriding it. First definition wins now, which is the
 * rule that lets a harness mean what it says. */
void recomp_trace_enter(const char *name, uint32_t va);
#ifndef RECOMP_TRACE_ENTER
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
#endif
void recomp_trace_exit(const char *name, uint32_t va);
#ifndef RECOMP_TRACE_EXIT
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
#endif
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#if defined(RECOMP_GPU_OWNERSHIP)
/* Guest-memory ownership seam.
 *
 * Every translated memory access in this header resolves its address through
 * XBOX_PTR: the scalar accessors, the signed and floating forms, the
 * FS-segmented forms, the XMM lane helpers, the locked read-modify-writes, and
 * the MEM32 lvalues the lifter emits for `rep movs`/`rep stos`.  That makes it
 * the one boundary wide enough to hold an ownership check that block
 * operations cannot slip past -- which is the whole reason the check is here
 * and not in the store helper below, where a load would never see it.
 *
 * The disabled path is one indexed byte load and a not-taken branch.  The map
 * is only non-empty while a render target is actually resident on the GPU, so
 * outside those granules this costs the same for a stack slot as it always
 * did.  See src/kernel/recomp_gpu_own.h.
 */
#define RECOMP_GPU_OWN_SHIFT 16
#define RECOMP_GPU_OWN_SLOTS (1u << (32 - RECOMP_GPU_OWN_SHIFT))
extern unsigned char g_recomp_gpu_own_map[RECOMP_GPU_OWN_SLOTS];
uintptr_t recomp_gpu_own_reconcile(uint32_t guest_va);

static inline uintptr_t recomp_guest_ptr(uint32_t va) {
    if (g_recomp_gpu_own_map[va >> RECOMP_GPU_OWN_SHIFT])
        return recomp_gpu_own_reconcile(va);
    return (uintptr_t)va + g_xbox_mem_offset;
}
#define XBOX_PTR(addr) recomp_guest_ptr((uint32_t)(addr))
#else
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#endif

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

/* Explicit translated-store seam.
 *
 * MEM8/MEM16/MEM32 must remain lvalues because generated reads use the same
 * spelling.  A callback hidden in those macros could not distinguish a read
 * from a write.  Store lowering therefore calls these helpers explicitly and
 * carries the original guest instruction PC with it.
 *
 * The runtime-enabled form has one predictable disabled-path branch. Builds
 * that consume this header without xbox_kernel (the conformance harnesses)
 * retain direct stores and need no tracer symbols. */
#if defined(RECOMP_MEM_WATCH_RUNTIME)
extern int g_recomp_mem_watch_enabled;
void recomp_mem_watch_guest_store(uint32_t guest_pc, uint32_t guest_function,
                                  uint32_t guest_va, unsigned width,
                                  volatile void *host_ptr, uint64_t new_value);

#define RECOMP_MEM_WRITE_HELPER(bits, type, pc, function, addr, value) do { \
    uint32_t _mw_addr = (uint32_t)(addr);                                  \
    type _mw_value = (type)(value);                                        \
    volatile type *_mw_ptr = (volatile type *)XBOX_PTR(_mw_addr);          \
    if (g_recomp_mem_watch_enabled)                                        \
        recomp_mem_watch_guest_store((uint32_t)(pc), (uint32_t)(function), \
                                     _mw_addr,                              \
                                     (bits) / 8u, (volatile void *)_mw_ptr, \
                                     (uint64_t)_mw_value);                 \
    else                                                                   \
        *_mw_ptr = _mw_value;                                              \
} while (0)
#else
#define RECOMP_MEM_WRITE_HELPER(bits, type, pc, function, addr, value) do { \
    (void)(pc); (void)(function); MEM##bits(addr) = (type)(value);           \
} while (0)
#endif

#define RECOMP_MEM_WRITE8(pc, function, addr, value)  \
    RECOMP_MEM_WRITE_HELPER(8, uint8_t, pc, function, addr, value)
#define RECOMP_MEM_WRITE16(pc, function, addr, value) \
    RECOMP_MEM_WRITE_HELPER(16, uint16_t, pc, function, addr, value)
#define RECOMP_MEM_WRITE32(pc, function, addr, value) \
    RECOMP_MEM_WRITE_HELPER(32, uint32_t, pc, function, addr, value)

static inline void RECOMP_MEM_WRITE64(uint32_t pc, uint32_t function,
                                      uint32_t addr, uint64_t value) {
#if defined(RECOMP_MEM_WATCH_RUNTIME)
    volatile uint64_t *ptr = (volatile uint64_t *)XBOX_PTR(addr);
    if (g_recomp_mem_watch_enabled)
        recomp_mem_watch_guest_store(pc, function, addr, 8,
                                     (volatile void *)ptr, value);
    else
        *ptr = value;
#else
    (void)pc;
    (void)function;
    *(volatile uint64_t *)XBOX_PTR(addr) = value;
#endif
}

static inline void RECOMP_MEM_WRITEF(uint32_t pc, uint32_t function,
                                     uint32_t addr, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    RECOMP_MEM_WRITE32(pc, function, addr, bits);
}

static inline void RECOMP_MEM_WRITED(uint32_t pc, uint32_t function,
                                     uint32_t addr, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    RECOMP_MEM_WRITE64(pc, function, addr, bits);
}

/** FS-segmented accesses retain their per-thread segment base. */
#define FS_MEM8(addr)  MEM8(g_fs_base + (uint32_t)(addr))
#define FS_MEM16(addr) MEM16(g_fs_base + (uint32_t)(addr))
#define FS_MEM32(addr) MEM32(g_fs_base + (uint32_t)(addr))
#define FS_SMEM8(addr)  SMEM8(g_fs_base + (uint32_t)(addr))
#define FS_SMEM16(addr) SMEM16(g_fs_base + (uint32_t)(addr))
#define FS_SMEM32(addr) SMEM32(g_fs_base + (uint32_t)(addr))
#define FS_SMEM64(addr) SMEM64(g_fs_base + (uint32_t)(addr))

/* LOCK XADD is an indivisible read/modify/write and returns the destination's
 * old value. Sequential consistency matches x86's locked-instruction ordering
 * and is available on every compiler supported by the runtime. */
static inline uint8_t RECOMP_ATOMIC_XADD8(uint32_t addr, uint8_t value) {
#if defined(_MSC_VER)
    return (uint8_t)_InterlockedExchangeAdd8(
        (volatile char *)XBOX_PTR(addr), (char)value);
#else
    return __atomic_fetch_add(
        (volatile uint8_t *)XBOX_PTR(addr), value, __ATOMIC_SEQ_CST);
#endif
}

static inline uint16_t RECOMP_ATOMIC_XADD16(uint32_t addr, uint16_t value) {
#if defined(_MSC_VER)
    return (uint16_t)_InterlockedExchangeAdd16(
        (volatile short *)XBOX_PTR(addr), (short)value);
#else
    return __atomic_fetch_add(
        (volatile uint16_t *)XBOX_PTR(addr), value, __ATOMIC_SEQ_CST);
#endif
}

static inline uint32_t RECOMP_ATOMIC_XADD32(uint32_t addr, uint32_t value) {
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedExchangeAdd(
        (volatile long *)XBOX_PTR(addr), (long)value);
#else
    return __atomic_fetch_add(
        (volatile uint32_t *)XBOX_PTR(addr), value, __ATOMIC_SEQ_CST);
#endif
}

/* LOCK CMPXCHG. Returns the destination's OLD value and reports through *ok
 * whether the exchange happened, which is the ZF the instruction leaves.
 *
 * On the GCC/Clang side __atomic_compare_exchange_n overwrites its expected
 * operand with the actual value when it fails and leaves it alone when it
 * succeeds, so `_exp` is the old value either way -- which is exactly what
 * x86 loads into the accumulator on failure. */
static inline uint8_t RECOMP_ATOMIC_CMPXCHG8(uint32_t addr, uint8_t expected,
                                             uint8_t desired, int *ok) {
#if defined(_MSC_VER)
    uint8_t old = (uint8_t)_InterlockedCompareExchange8(
        (volatile char *)XBOX_PTR(addr), (char)desired, (char)expected);
    *ok = (old == expected);
    return old;
#else
    uint8_t _exp = expected;
    *ok = __atomic_compare_exchange_n((volatile uint8_t *)XBOX_PTR(addr),
                                      &_exp, desired, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return _exp;
#endif
}

static inline uint16_t RECOMP_ATOMIC_CMPXCHG16(uint32_t addr, uint16_t expected,
                                               uint16_t desired, int *ok) {
#if defined(_MSC_VER)
    uint16_t old = (uint16_t)_InterlockedCompareExchange16(
        (volatile short *)XBOX_PTR(addr), (short)desired, (short)expected);
    *ok = (old == expected);
    return old;
#else
    uint16_t _exp = expected;
    *ok = __atomic_compare_exchange_n((volatile uint16_t *)XBOX_PTR(addr),
                                      &_exp, desired, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return _exp;
#endif
}

static inline uint32_t RECOMP_ATOMIC_CMPXCHG32(uint32_t addr, uint32_t expected,
                                               uint32_t desired, int *ok) {
#if defined(_MSC_VER)
    uint32_t old = (uint32_t)_InterlockedCompareExchange(
        (volatile long *)XBOX_PTR(addr), (long)desired, (long)expected);
    *ok = (old == expected);
    return old;
#else
    uint32_t _exp = expected;
    *ok = __atomic_compare_exchange_n((volatile uint32_t *)XBOX_PTR(addr),
                                      &_exp, desired, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return _exp;
#endif
}

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* -- construction -- */

static inline RecompXmm XMM_ZERO(void) {
    RecompXmm r; r.q[0] = 0; r.q[1] = 0; return r;
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) {
    RecompXmm r = XMM_ZERO(); r.f[0] = v; return r;
}

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    RecompXmm r = XMM_ZERO(); r.d[0] = v; return r;
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    RecompXmm r = XMM_ZERO(); r.u[0] = bits; return r;
}

/* -- guest memory --
 * Addresses are guest VAs, so they go through MEM32 like every other
 * access. Done lane-wise, which is also unaligned-safe for movups. */

static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}

static inline void XMM_STORE(uint32_t addr, RecompXmm v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
}

static inline void recomp_xmm_store_half(uint32_t addr, RecompXmm src,
                                         int high) {
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
}

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r;
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = b.q[1]; r.q[1] = a.q[1]; return r;
}

/* -- packed arithmetic -- */

#define RECOMP_XMM_LANEWISE(name, expr)                                   \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when the lanes are unordered or
 * equal -- that is the hardware's tie-break, not a C fmin/fmax. */
RECOMP_XMM_LANEWISE(XMM_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

#define RECOMP_XMM_BITWISE(name, expr)                                    \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.u[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_BITWISE(XMM_AND,  a.u[i] & b.u[i])
RECOMP_XMM_BITWISE(XMM_OR,   a.u[i] | b.u[i])
RECOMP_XMM_BITWISE(XMM_XOR,  a.u[i] ^ b.u[i])
/* ANDNPS is ~dst & src, not dst & ~src. */
RECOMP_XMM_BITWISE(XMM_ANDN, (~a.u[i]) & b.u[i])

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
RECOMP_XMM_BITWISE(XMM_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RecompXmm a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. */
static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

/* The n == 0 case has to be taken before the shifts: after `n &= 31` the
 * complement is 32, and shifting a uint32_t by 32 is undefined in C even
 * though the x86 instruction is simply a no-op there. */
static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    if (n == 0) return val;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    if (n == 0) return val;
    return (val >> n) | (val << (32 - n));
}

/* Rotate THROUGH CARRY, at the operand's own width.
 *
 * These are a rotate of WIDTH+1 bits, not WIDTH: the carry flag is the extra
 * bit. So a byte rcl is a 9-bit rotate, a word one 17-bit and a dword one
 * 33-bit, and the count is reduced modulo that -- which is why they cannot be
 * expressed through ROL32/ROR32, and why one 32-bit helper cannot serve all
 * three widths.
 *
 * `shr high, 1` followed by `rcr low, 1` is how MSVC shifts a 64-bit value
 * right by one, and JSRF reaches four helpers built that way. While rcr was
 * emitted as a no-op comment the low word was never shifted at all, so the
 * shift returned a number half of which had not moved.
 *
 * x86 masks the count to 5 bits BEFORE the modulo, so `rcl al, 32` is a count
 * of 0 and not of 32 % 9. A count of 0 leaves value and flag untouched. */
static inline uint32_t RECOMP_RCR(uint32_t val, int n, int *cf, int bits) {
    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    val &= mask;
    n = (n & 31) % (bits + 1);
    while (n--) {
        int lsb = (int)(val & 1u);
        val = ((val >> 1) | ((uint32_t)(*cf & 1) << (bits - 1))) & mask;
        *cf = lsb;
    }
    return val;
}

static inline uint32_t RECOMP_RCL(uint32_t val, int n, int *cf, int bits) {
    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    val &= mask;
    n = (n & 31) % (bits + 1);
    while (n--) {
        int msb = (int)((val >> (bits - 1)) & 1u);
        val = ((val << 1) | (uint32_t)(*cf & 1)) & mask;
        *cf = msb;
    }
    return val;
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/* Architectural ADD flags used by XADD.  Keeping the six defined arithmetic
 * flags in a compact EFLAGS-shaped value lets a later Jcc/SETcc/CMOVcc consume
 * them even when it begins another translated basic block. */
#define RECOMP_EFLAG_CF 0x00000001u
#define RECOMP_EFLAG_PF 0x00000004u
#define RECOMP_EFLAG_AF 0x00000010u
#define RECOMP_EFLAG_ZF 0x00000040u
#define RECOMP_EFLAG_SF 0x00000080u
#define RECOMP_EFLAG_OF 0x00000800u

#define RECOMP_EFLAGS_CF(f) (((uint32_t)(f) & RECOMP_EFLAG_CF) != 0)
#define RECOMP_EFLAGS_PF(f) (((uint32_t)(f) & RECOMP_EFLAG_PF) != 0)
#define RECOMP_EFLAGS_AF(f) (((uint32_t)(f) & RECOMP_EFLAG_AF) != 0)
#define RECOMP_EFLAGS_ZF(f) (((uint32_t)(f) & RECOMP_EFLAG_ZF) != 0)
#define RECOMP_EFLAGS_SF(f) (((uint32_t)(f) & RECOMP_EFLAG_SF) != 0)
#define RECOMP_EFLAGS_OF(f) (((uint32_t)(f) & RECOMP_EFLAG_OF) != 0)

static inline uint32_t recomp_add_eflags(uint32_t lhs, uint32_t rhs,
                                         uint32_t result, unsigned bits) {
    uint32_t mask = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t sign = bits == 8 ? 0x80u : bits == 16 ? 0x8000u : 0x80000000u;
    uint32_t flags = 0;
    lhs &= mask;
    rhs &= mask;
    result &= mask;
    if ((uint64_t)lhs + (uint64_t)rhs > (uint64_t)mask)
        flags |= RECOMP_EFLAG_CF;
    if (RECOMP_PARITY8(result))
        flags |= RECOMP_EFLAG_PF;
    if ((lhs ^ rhs ^ result) & 0x10u)
        flags |= RECOMP_EFLAG_AF;
    if (result == 0)
        flags |= RECOMP_EFLAG_ZF;
    if (result & sign)
        flags |= RECOMP_EFLAG_SF;
    if ((~(lhs ^ rhs) & (lhs ^ result) & sign) != 0)
        flags |= RECOMP_EFLAG_OF;
    return flags;
}

#define RECOMP_ADD_FLAGS8(a, b, r)  recomp_add_eflags((a), (b), (r), 8)
#define RECOMP_ADD_FLAGS16(a, b, r) recomp_add_eflags((a), (b), (r), 16)
#define RECOMP_ADD_FLAGS32(a, b, r) recomp_add_eflags((a), (b), (r), 32)

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Bit scan (bsf/bsr)
 *
 * Index of the lowest (BSF32) or highest (BSR32) set bit. The Xbox D3D
 * library's Log2 helper is a bare `bsf eax, ecx`, so a title's texture and
 * surface sizes are computed through these.
 *
 * A zero operand is the caller's problem, not theirs: x86 leaves the
 * destination register unmodified when the source is zero, so the lifted code
 * guards the call and these are never reached with v == 0. Returning some
 * stand-in value here instead would put a number in the destination that the
 * hardware never puts there.
 * ================================================================ */

static inline uint32_t BSF32(uint32_t v) {
    uint32_t i = 0;
    while (!((v >> i) & 1u)) i++;
    return i;
}

static inline uint32_t BSR32(uint32_t v) {
    uint32_t i = 31;
    while (!((v >> i) & 1u)) i--;
    return i;
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ABI_CALL - call a resolved target, optionally checking the ABI.
 *
 * ebx, esi and edi are callee-saved on x86, and the recompiler keeps them in
 * globals rather than on the host's stack. So a lifted function that never
 * reaches its own epilogue -- the usual cause is a decode that lost sync on
 * embedded data -- does not merely lose its own state, it silently corrupts
 * every caller's.
 *
 * That failure is invisible at the crash site: the caller carries on with a
 * wrong loop cursor and simply does less work. Half-Life 2's C++ static
 * initialiser walks 5,305 constructors with esi as the cursor and edi as the
 * limit; a single callee returning with those changed ended the walk early and
 * left Source with no registered interfaces, with no error anywhere.
 *
 * Build with -DRECOMP_ABI_CHECK to have every indirect call verify them and
 * name the first offenders. esp is deliberately not checked: the convention
 * decides whether the callee pops arguments, so there is no single correct
 * value -- but there is one invariant that holds under every convention:
 * the callee at least pops its own return address, so esp must come back
 * at least 4 higher than it went in. Coming back lower means the epilogue
 * never ran, which is the failure this exists to catch.
 *
 * Covers direct calls as well as indirect: tools/recomp emits every direct
 * call through this macro, which expands to a plain call when the flag is
 * off. That matters because CRT and static-initialiser paths -- where these
 * clobbers actually bite -- are almost entirely direct calls.
 *
 * HOW the target was reached is recorded with the violation, because the three
 * ways do not mean the same thing and the count is uninterpretable pooled.
 * RECOMP_ITAIL is an indirect *tail jump*: no return address is pushed, the
 * frame belongs to the jumping function, and a shared epilogue reached that way
 * is SUPPOSED to restore the caller's caller's ebx/esi/edi. Checked as though
 * it were a call, that reads as a violation every time -- false by
 * construction, and indistinguishable from a real one until the kind is
 * printed beside it. 'C' is a direct call, 'I' an indirect call, 'T' a tail
 * jump; the dedup key is the pair, so a target reached both ways is reported
 * once for each.
 */
#ifdef RECOMP_ABI_CHECK
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0, int kind);
#define RECOMP_ABI_CALL_K(va, fn, kind) do { \
    uint32_t _ab = g_ebx, _as = g_esi, _ad = g_edi, _ap = g_esp; \
    (fn)(); \
    if (g_ebx != _ab || g_esi != _as || g_edi != _ad || g_esp < _ap + 4) \
        recomp_abi_violation_log((va), _ab, _as, _ad, _ap, (kind)); \
} while(0)
#else
#define RECOMP_ABI_CALL_K(va, fn, kind) (fn)()
#endif
/* Direct calls are the overwhelming majority and the lifter emits thousands of
 * them, so they keep the short spelling. */
#define RECOMP_ABI_CALL(va, fn) RECOMP_ABI_CALL_K((va), (fn), 'C')

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * RECOMP_ICALL_IS_CODE skips garbage VAs that come from uninitialized vtable
 * pointers. Kernel thunks at 0xFE000000+ are never blocked.
 *
 * Nothing to customize: the range comes from g_xbox_code_lo/g_xbox_code_hi,
 * which the memory layout fills in from the sections it actually mapped. This
 * used to say "change the VA range check to match your game's code range",
 * left over from a hardcoded 0x00400000 cutoff that was only ever right for
 * one title. Editing this header per project is no longer a thing.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL_K(_va, _fn, 'I'); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL_K(_va, _fn, 'I'); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL_K(_va, _fn, 'T'); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/* ================================================================
 * MMX register file
 *
 * The Xbox is a Pentium III and every XDK codec leans on MMX: the WMV
 * decoder's IDCT and motion compensation are almost nothing else. Modelled the
 * same way as RecompXmm -- a union of lane views over one 64-bit register --
 * because that is what the instructions are: the same bits read as bytes,
 * words or dwords.
 *
 * mm0..mm7 alias the x87 stack on real hardware. Nothing here does, and
 * nothing needs to: a title that interleaves the two calls emms between, and
 * emms is a no-op for us. Modelling the aliasing would mean giving up the
 * separate x87 model that the FPU work depends on, to reproduce a hazard the
 * hardware exists to let software avoid.
 * ================================================================ */

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

static inline RecompMmx MMX_ZERO(void) { RecompMmx r; r.q = 0; return r; }

/* CVTPS2PI follows MXCSR; CVTTPS2PI truncates regardless of its rounding mode.
 * Use SSE scalar conversions to avoid touching the host x87/MMX register file.
 * Non-x86 hosts use their floating-point environment for rounding instead. */
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
#else
    double rounded = truncate ? trunc((double)v) : nearbyint((double)v);
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0))
        return (int32_t)0x80000000u;       /* integer indefinite */
    return (int32_t)rounded;
#endif
}

static inline RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
    RecompMmx r;
    r.d[0] = MMX_CVT_F2I(lo, truncate);
    r.d[1] = MMX_CVT_F2I(hi, truncate);
    return r;
}

/** cvtpi2ps: two signed dwords in, two singles out, into the LOW half of the
 * destination -- lanes 2 and 3 keep whatever they held. That detail is the
 * whole instruction: code that builds a float4 from two of these relies on
 * the first one surviving the second. */
static inline RecompXmm XMM_FROM_PI(RecompXmm dst, RecompMmx src)
{
    dst.f[0] = (float)src.d[0];
    dst.f[1] = (float)src.d[1];
    return dst;
}

static inline RecompMmx MMX_MEM(uint32_t addr) {
    RecompMmx r;
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
    return r;
}

static inline void MMX_STORE(uint32_t addr, RecompMmx v) {
    memcpy((void *)XBOX_PTR(addr), &v, 8);
}

static inline RecompMmx MMX_FROM32(uint32_t v) {
    RecompMmx r; r.q = 0; r.ud[0] = v; return r;
}

/* -- saturation helpers ---------------------------------------- */
static inline int16_t recomp_sat_i16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
static inline int8_t recomp_sat_i8(int32_t v) {
    return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}
static inline uint8_t recomp_sat_u8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

static inline uint16_t recomp_sat_u16(int32_t v) {
    return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
}

/* -- integer arithmetic, lane-wise, wrapping -------------------- */
#define RECOMP_MMX_BINOP(NAME, LANES, FIELD, EXPR)                      \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++) { (void)b; r.FIELD[i] = (EXPR); } \
        return r;                                                       \
    }

RECOMP_MMX_BINOP(MMX_PADDB, 8, b, (int8_t)((uint8_t)a.b[i] + (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDW, 4, w, (int16_t)((uint16_t)a.w[i] + (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDD, 2, d, (int32_t)((uint32_t)a.d[i] + (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PSUBB, 8, b, (int8_t)((uint8_t)a.b[i] - (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBW, 4, w, (int16_t)((uint16_t)a.w[i] - (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBD, 2, d, (int32_t)((uint32_t)a.d[i] - (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PADDSB, 8, b, recomp_sat_i8((int32_t)a.b[i] + b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDSW, 4, w, recomp_sat_i16((int32_t)a.w[i] + b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBSB, 8, b, recomp_sat_i8((int32_t)a.b[i] - b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBSW, 4, w, recomp_sat_i16((int32_t)a.w[i] - b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] + b.ub[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] - b.ub[i]))
RECOMP_MMX_BINOP(MMX_PADDUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] + b.uw[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] - b.uw[i]))
RECOMP_MMX_BINOP(MMX_PMULLW, 4, w, (int16_t)((int32_t)a.w[i] * b.w[i]))
RECOMP_MMX_BINOP(MMX_PMULHW, 4, w, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
RECOMP_MMX_BINOP(MMX_PAVGB, 8, ub, (uint8_t)(((int32_t)a.ub[i] + b.ub[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PAVGW, 4, uw, (uint16_t)(((int32_t)a.uw[i] + b.uw[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PMINSW, 4, w, (a.w[i] < b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PMAXSW, 4, w, (a.w[i] > b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PCMPEQB, 8, b, (int8_t)(a.b[i] == b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQW, 4, w, (int16_t)(a.w[i] == b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQD, 2, d, (int32_t)(a.d[i] == b.d[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTB, 8, b, (int8_t)(a.b[i] > b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTW, 4, w, (int16_t)(a.w[i] > b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTD, 2, d, (int32_t)(a.d[i] > b.d[i] ? -1 : 0))

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];
    r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];
    return r;
}

/* -- bitwise ---------------------------------------------------- */
static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* -- shifts -----------------------------------------------------
 * A count at or past the lane width gives zero for the logical shifts and a
 * full sign fill for the arithmetic ones. The count is the whole 64-bit
 * register, not one lane, and it is unsigned -- a negative-looking count is a
 * huge one, which saturates the same way.
 */
#define RECOMP_MMX_SHIFT(NAME, LANES, FIELD, WIDTH, OP)                 \
    static inline RecompMmx NAME(RecompMmx a, uint64_t cnt) {           \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++)                                   \
            r.FIELD[i] = (cnt >= (WIDTH)) ? 0 : (OP);                   \
        return r;                                                       \
    }
RECOMP_MMX_SHIFT(MMX_PSLLW, 4, uw, 16, (uint16_t)(a.uw[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLW, 4, uw, 16, (uint16_t)(a.uw[i] >> cnt))
RECOMP_MMX_SHIFT(MMX_PSLLD, 2, ud, 32, (uint32_t)(a.ud[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLD, 2, ud, 32, (uint32_t)(a.ud[i] >> cnt))

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q << cnt); return r;
}
static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q >> cnt); return r;
}
static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 16) ? 15 : cnt;
    for (i = 0; i < 4; i++) r.w[i] = (int16_t)(a.w[i] >> c);
    return r;
}
static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 32) ? 31 : cnt;
    for (i = 0; i < 2; i++) r.d[i] = (int32_t)(a.d[i] >> c);
    return r;
}

/* -- unpack / pack ---------------------------------------------- */
static inline RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i]; r.ub[i*2+1] = b.ub[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i+4]; r.ub[i*2+1] = b.ub[i+4]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i]; r.uw[i*2+1] = b.uw[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i+2]; r.uw[i*2+1] = b.uw[i+2]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[0]; r.ud[1] = b.ud[0]; return r;
}
static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1]; return r;
}
static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.b[i]     = recomp_sat_i8(a.w[i]);
    for (i = 0; i < 4; i++) r.b[i + 4] = recomp_sat_i8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.ub[i]     = recomp_sat_u8(a.w[i]);
    for (i = 0; i < 4; i++) r.ub[i + 4] = recomp_sat_u8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) r.w[i]     = recomp_sat_i16(a.d[i]);
    for (i = 0; i < 2; i++) r.w[i + 2] = recomp_sat_i16(b.d[i]);
    return r;
}

/* -- word insert / extract / shuffle ----------------------------- */
static inline RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.uw[i] = a.uw[(imm >> (i * 2)) & 3];
    return r;
}
static inline RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    RecompMmx r = a; r.uw[imm & 3] = (uint16_t)v; return r;
}
static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return (uint32_t)a.uw[imm & 3];
}
static inline uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (a.ub[i] & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++)
        s += (uint32_t)(a.ub[i] > b.ub[i] ? a.ub[i] - b.ub[i] : b.ub[i] - a.ub[i]);
    r.q = 0; r.uw[0] = (uint16_t)s;
    return r;
}


/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */
