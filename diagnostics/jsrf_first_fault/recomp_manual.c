/**
 * Manual function overrides and ICALL diagnostics.
 *
 * Vendored here on 14 Sep 2026. It used to be pulled from a sibling checkout
 * outside the repository, via a JSRF_STOCK_DIR that resolved to
 * ../jsrf_stock_test -- so a fresh clone failed in CMake on a path the person
 * cloning had never heard of, and the documented build worked only on the one
 * machine that happened to have that directory. The file is 130 lines of this
 * project's own code with no game data in it, and the two include directories
 * that came with it contained no headers at all.
 *
 * Based on templates/new-game/src/recomp_manual.c. Provides the game-side
 * integration points required by the runtime (see include/xbox/xboxrecomp.h):
 *   - recomp_lookup_manual()  : hand-written overrides (none yet -> NULL)
 *   - recomp_icall_fail_log() : log when an indirect call target can't resolve
 *   - ICALL trace ring buffer globals (used by the RECOMP_ICALL macro)
 *
 * The only deviation from the template: recomp_icall_fail_log() is rate-limited
 * (prints the first 100 failures, then counts silently) so a bring-up run with
 * many unresolved vtable targets does not flood stderr. Diagnostic aid only;
 * no Xbox semantics involved.
 */

#include <stdio.h>
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>

/* Declares the g_icall_* externs (defined in the runtime) and recomp_func_t.
 * RECOMP_GENERATED_CODE is NOT defined here, so the eax->g_eax register aliases
 * stay inactive in this hand-written file. */
#include "recomp_types.h"

/* ── ICALL trace ring buffer ─────────────────────────────────────────────── */
/* The g_icall_trace / g_icall_trace_idx / g_icall_count globals are DEFINED in
 * the runtime's xbox_memory_layout.c (the "xbox_memory_layout.c pattern" the
 * template documents). Do NOT redefine them here, or the link fails with
 * duplicate symbols. They are declared extern in recomp_types.h and written by
 * the RECOMP_ICALL macro; recomp_icall_fail_log() below reads them. */

typedef void (*recomp_func_t)(void);

/* ── Manual function overrides ───────────────────────────────────────────── */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /* No hand-written overrides yet: fall through to the generated dispatch. */
    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging (rate-limited) ────────────────────────────────── */

static uint64_t s_fail_printed = 0;
#define FAIL_PRINT_LIMIT 100

/* docs/pipeline/06-debugging.md calls a failed indirect call "the most common
 * crash type" and prescribes two things for it, both of which this trimmed
 * copy had dropped: the CALLER address, and the ring of recent targets.
 *
 * Without the caller there is nothing to map back to a function, so the log
 * says a bad pointer was called and not who called it -- which is the one fact
 * that matters. Without the ring there is no execution context either.
 *
 * The caller is a native address in this binary. Map it the way the doc says:
 * a .map file under MSVC, or on a mingw build
 *     x86_64-w64-mingw32-addr2line -f -e jsrf_first_fault.exe <caller>
 * which resolves straight to the generated sub_XXXXXXXX. */
#if defined(_MSC_VER)
#  include <intrin.h>
#  define RECOMP_CALLER_ADDRESS() _ReturnAddress()
#else
#  define RECOMP_CALLER_ADDRESS() __builtin_return_address(0)
#endif

void recomp_icall_fail_log(uint32_t va)
{
    if (s_fail_printed < FAIL_PRINT_LIMIT) {
        void *caller = RECOMP_CALLER_ADDRESS();
        int i;
        s_fail_printed++;
        fprintf(stderr,
                "[ICALL] Failed to resolve VA 0x%08X caller=%p (total calls: %llu)\n",
                va, caller, (unsigned long long)g_icall_count);
        /* Last 16 targets, oldest first. The valid entries before the garbage
         * one are the execution context the doc asks for. */
        fprintf(stderr, "[ICALL]   recent targets:");
        for (i = 0; i < ICALL_TRACE_SIZE; i++) {
            uint32_t t = g_icall_trace[(g_icall_trace_idx - ICALL_TRACE_SIZE + i)
                                       & (ICALL_TRACE_SIZE - 1)];
            if (t) fprintf(stderr, " %08X", t);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        if (s_fail_printed == FAIL_PRINT_LIMIT) {
            fprintf(stderr, "[ICALL] ... further failures suppressed (counted only)\n");
        }
    }
}

/* ── Indirect calls dropped as not-code ──────────────────────────────────── */
/*
 * RECOMP_ICALL drops a target outside the code range rather than calling into
 * data. That is the right call and it used to be silent, which is not: the
 * macro sets eax = 0 and continues, which looks exactly like a function that
 * returned early. A null or wild function pointer inside a per-frame loop then
 * presents as a title that runs and quietly does nothing at all.
 *
 * Rate-limited per address at 1, 10, 100, 1000 rather than once, because the
 * progression carries the diagnosis: a single line says one pointer was
 * skipped, the same address climbing says it is being skipped every frame, and
 * those two need different responses.
 */
#define NOT_CODE_SLOTS 32

void recomp_icall_not_code_log(uint32_t va)
{
    static uint32_t addr[NOT_CODE_SLOTS];
    static uint64_t hits[NOT_CODE_SLOTS];
    static int used;
    int i;

    for (i = 0; i < used; i++) {
        if (addr[i] == va) break;
    }
    if (i == used) {
        if (used >= NOT_CODE_SLOTS) return;   /* bounded; the busy ones are in */
        addr[used] = va;
        hits[used] = 0;
        used++;
    }
    hits[i]++;
    if (hits[i] == 1 || hits[i] == 10 || hits[i] == 100 || hits[i] == 1000
        || (hits[i] % 10000) == 0) {
        fprintf(stderr, "[ICALL] skipped not-code target 0x%08X (%llu times)\n",
                va, (unsigned long long)hits[i]);
        fflush(stderr);
    }
}
