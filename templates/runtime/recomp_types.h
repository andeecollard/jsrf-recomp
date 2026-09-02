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

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

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
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
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
extern RECOMP_TLS uint32_t g_seh_ebp;
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

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)

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

/* An indirect call dropped because its target is not in a code range.
 *
 * Dropping it is right -- calling a data address is worse than not calling --
 * but dropping it SILENTLY is not: eax = 0 and carry on is indistinguishable
 * from a function that returned early, so a wild or null function pointer in a
 * loop presents as a title that quietly does nothing.
 *
 * Rate-limited per address at 1, 10, 100, 1000. The progression is the point:
 * one line says a pointer was skipped, the sequence says it is being skipped
 * every frame, and those need different responses. */
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
void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
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
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

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

/* MMX registers.
 *
 * Not modelled at all until now, which was not a harmless omission: the D3D
 * block copy at sub_00198FD0 is `movq mm0..mm7, [esi+n]` followed by
 * `movntq [edi+n], mm0..mm7`, 64 bytes an iteration. The loads fell out of the
 * SSE handler as an "SSE: movq" comment and the stores were left as a "TODO:
 * movntq" comment, so the memcpy copied its alignment prologue and epilogue and
 * silently dropped every 64-byte block in between.
 *
 * Per-thread like the rest of the register file. The x87/MMX aliasing the real
 * hardware has is NOT modelled: nothing in the corpus interleaves the two, and
 * pretending otherwise would cost more than it buys. */
extern RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3,
                           g_mm4, g_mm5, g_mm6, g_mm7;

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

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
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
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * The range check (0x00400000 to 0xFE000000) skips garbage VAs that
 * come from uninitialized vtable pointers. Adjust this range based
 * on your game's .text section boundaries. Kernel thunks at
 * 0xFE000000+ must NOT be blocked.
 *
 * CUSTOMIZE: Change the VA range check to match your game's code range.
 * Your .text section typically spans 0x00010000 to ~0x003XXXXX.
 * Any VA outside .text and below 0xFE000000 is likely garbage.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
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
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
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
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

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
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7

/** 64-bit memory access, for MMX loads and stores. */
#define MEM64(addr) (*(volatile uint64_t *)XBOX_PTR(addr))

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
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
