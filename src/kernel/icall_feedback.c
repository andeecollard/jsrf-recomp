/*
 * Indirect-branch target feedback.
 *
 * Records which guest addresses the title actually reaches through indirect
 * branches, so the next codegen run can seed function detection with them
 * instead of guessing. This is the local equivalent of the two inputs
 * Microsoft's own recompiler takes -- VirtualDispatchTraceFiles (recorded
 * indirect-branch target sets) and UpdateEnlightenments (a persisted analysis
 * database the compiler rewrites every build). See
 * docs/technical/ms-fusion-codegen-teardown.md.
 *
 * Static analysis cannot see where a vtable call goes. Running the title can.
 * tools/recomp/analyze_unresolved.py currently classifies unresolved targets by
 * where they land relative to known functions, which is inference; this is
 * measurement.
 *
 * Opt-in: define RECOMP_ICALL_FEEDBACK. Without it this file compiles to
 * nothing and the RECOMP_ICALL_OBSERVE hook in recomp_types.h expands to
 * (void)0, so release builds pay neither the store nor the 8 MiB.
 *
 * Cost when enabled: one byte OR per indirect branch, into a flat array indexed
 * by guest VA. A byte array rather than a bitmap because a byte store needs no
 * read-modify-write, so concurrent recompiled threads cannot lose each other's
 * writes -- and 8 MiB is not worth a CAS loop. Dedup is free: the same target
 * hit a million times is still one byte.
 */

#include <stdint.h>

#ifdef RECOMP_ICALL_FEEDBACK

#include <stdio.h>
#include <stdlib.h>

#include "recomp_icall_feedback.h"

/* One byte per guest VA in the image window. Zero-initialised in BSS; the OS
 * only commits the pages actually touched, so a title that reaches 4k distinct
 * targets does not resident 8 MiB. */
volatile unsigned char g_icall_seen[RECOMP_ICALL_FB_SIZE];

const char *recomp_icall_feedback_path(void)
{
    /* Resolved once. The periodic dump, the crash handler and atexit must all
     * agree about where the run's evidence went, and a getenv re-read could
     * disagree if anything ever called setenv mid-run. */
    static const char *cached;
    if (!cached) {
        const char *e = getenv("RECOMP_ICALL_FEEDBACK_PATH");
        cached = (e && *e) ? e : RECOMP_ICALL_FEEDBACK_PATH;
    }
    return cached;
}

void recomp_icall_feedback_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        /* ONCE, NOT ONCE PER REPORT. This fires from the periodic report, so
         * the old message printed 91 times in one session -- frequent enough
         * to scroll past and vague enough to ignore, which is how a dead
         * feedback loop survived three days. Say it once, say what it means,
         * and name the variable that fixes it. */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr,
                    "[icall-feedback] CANNOT WRITE %s -- no indirect-branch"
                    " targets from this run will be recorded, and the"
                    " persisted database will not advance. A GUI-launched app"
                    " bundle has no writable working directory, which is the"
                    " usual cause. Set RECOMP_ICALL_FEEDBACK_PATH to an"
                    " absolute path your launcher can write."
                    " (Further failures this run are suppressed.)\n", path);
        }
        return;
    }

    /* Plain text, one record per line: VA and the flags observed for it.
     * Deliberately not JSON -- this is written from a possibly-crashing
     * process, so a truncated file must still be parseable line by line.
     * tools/recomp/icall_feedback.py merges it. */
    unsigned long resolved = 0, unresolved = 0;
    fprintf(f, "# icall-feedback v1\n");
    fprintf(f, "# va flags   (1=resolved, 2=unresolved, 3=both)\n");
    for (uint32_t off = 0; off < RECOMP_ICALL_FB_SIZE; off++) {
        unsigned char v = g_icall_seen[off];
        if (!v)
            continue;
        fprintf(f, "%08X %u\n", (unsigned)(RECOMP_ICALL_FB_BASE + off), v);
        if (v & RECOMP_ICALL_SEEN_RESOLVED)   resolved++;
        if (v & RECOMP_ICALL_SEEN_UNRESOLVED) unresolved++;
    }
    fclose(f);

    fprintf(stderr, "[icall-feedback] %s: %lu resolved, %lu unresolved targets\n",
            path, resolved, unresolved);
}

static void dump_at_exit(void)
{
    recomp_icall_feedback_dump(recomp_icall_feedback_path());
}

void recomp_icall_feedback_init(void)
{
    /* atexit only fires on a clean exit. A title killed by a watchdog timeout
     * (run.sh uses `timeout`, which is exit 124) never reaches it, which is why
     * the crash handler dumps too -- and why, if a title neither exits nor
     * crashes, you must call the dump yourself from wherever you decide the run
     * is over. There is deliberately no timer thread doing it behind your back. */
    atexit(dump_at_exit);
}

#else  /* !RECOMP_ICALL_FEEDBACK */

/* ISO C forbids an empty translation unit. */
typedef int recomp_icall_feedback_disabled;

#endif
