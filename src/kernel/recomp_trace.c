/**
 * Function tracing for recompiled code.
 *
 * recomp_types.h declares these and tools.recomp emits calls to them under
 * --trace-functions, but until now nothing defined them: the definitions lived
 * in one game project, so enabling tracing anywhere else failed at link time
 * with three unresolved symbols and no hint that the fix was to go and copy a
 * file. They belong with the runtime that declares them.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = getenv("RECOMP_TRACE_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    if (!trace_budget()) return;
    /* The return address as well as the registers: at entry it is still at
     * [esp], and it names the call site, which is the thing a trace of "who
     * reached this" actually needs. Reading a guest stack dump for it works
     * only when the frames above are still live. */
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va,
            *(const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset()),
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);

    /* The stack arguments too, when asked. Registers alone do not say which
     * argument arrived null, and for a function with a long argument list,
     * counting pushes back from the call site is guesswork. */
    if (getenv("RECOMP_TRACE_ARGS")) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int n = atoi(getenv("RECOMP_TRACE_ARGS"));
        int i;

        if (n <= 0 || n > 32)
            n = 8;
        fprintf(stderr, "         args:");
        for (i = 1; i <= n; i++)
            fprintf(stderr, " %d=%08X", i,
                    *(const uint32_t *)(mem + g_esp + i * 4));
        fprintf(stderr, "\n");
        /* And the object eax points at. A matrix of NaNs says the maths went
         * wrong; whether its inputs were already zero says whether the maths
         * is at fault or the data behind it was never built. */
        /* Follow the pointer arguments one level. A matrix that arrives as
         * NaN was copied from somewhere, and the object it came from is what
         * needs looking at -- the value alone says only that it is wrong. */
        if (getenv("RECOMP_TRACE_DEREF")) {
            for (i = 1; i <= n; i++) {
                uint32_t a = *(const uint32_t *)(mem + g_esp + i * 4);
                int k;
                if (a < 0x00010000u || a >= 0x04000000u)
                    continue;
                fprintf(stderr, "         arg%d -> [%08X]:", i, a);
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %08X",
                            *(const uint32_t *)(mem + a + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (g_eax > 0x00010000u && g_eax < 0x04000000u) {
            fprintf(stderr, "         [eax=%08X]:", g_eax);
            for (i = 0; i < 24; i++)
                fprintf(stderr, " %08X",
                        *(const uint32_t *)(mem + g_eax + i * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    if (!trace_budget()) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (!trace_budget()) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}
