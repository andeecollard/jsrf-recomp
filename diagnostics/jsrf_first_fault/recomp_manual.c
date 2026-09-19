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
#include <stdlib.h>

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

/* Defined further down, used by the tail-jump logger above it. Declared here
 * rather than in recomp_types.h because they are this harness's log policy,
 * not part of the runtime contract every title has to implement.
 * diagnostics/jsrf_first_fault/icall_runaway_test.c drives all three. */
int recomp_icall_log_cadence(uint64_t hits);
uint64_t recomp_icall_runaway_threshold(void);
int recomp_icall_runaway_aborts(void);

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

/* The last VA the dispatch table could not resolve, kept whether or not the
 * line was printed. A runaway NULL spin is the DOWNSTREAM symptom of one of
 * these: the unresolved call is skipped, its caller resumes on registers the
 * skipped callee never restored, and the wreckage eventually calls through a
 * zeroed pointer forever. The two events are minutes and a million log lines
 * apart, and joining them by hand is how 0x00114B66 stayed a mystery across
 * ninety-three runs. See recomp_icall_not_code_log(). */
static uint32_t s_last_unresolved_va;
static uint64_t s_last_unresolved_at;
static uint64_t s_unresolved_total;

void recomp_icall_fail_log(uint32_t va)
{
    s_last_unresolved_va = va;
    s_last_unresolved_at = g_icall_count;
    s_unresolved_total++;

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

/* ── Indirect TAIL JUMPS that resolved to nothing ────────────────────────── */
/*
 * This is not a vtable miss and must not read like one.
 *
 * RECOMP_ITAIL's failure path does `g_esp += 4` and returns, which is right
 * for a real tail jump: the jumping function has already run its epilogue and
 * esp is resting on the return address. Nearly every one that FAILS is not
 * that. It is an MSVC switch dispatch -- `jmp [reg*4 + table]` -- whose arms
 * the function detector carved out of the enclosing body. tools/recomp's
 * _analyze_switch_table then needs at least two arms inside [func_start,
 * func_end) to lift it as a switch, finds fewer because the carve moved the
 * end, and emits a bare indirect tail jump instead. The arm is not a function,
 * nothing is registered at its VA, the jump resolves to nothing, and the
 * runtime pops four bytes off a frame that is still fully live.
 *
 * So: work out how wrong the stack now is and say so. The dword this failure
 * is about to return through has to be a return address if the epilogue ran.
 * Reading it costs nothing and answers the question directly -- if it is not
 * code, the epilogue did NOT run and the frame is still there. Walking up for
 * the nearest dword that IS code then measures the frame that has just been
 * stranded: the leak, in bytes, at the moment it happens.
 *
 * Measured on JSRF 19 Sep 2026: 196 such dispatch sites through 84 tables,
 * 607 of 711 arms with no translated body. sub_000A5B60 strands 0x60 bytes
 * this way, and that arithmetic closes to the byte against the esp in all
 * four of the player's skating-crash logs.
 */
#define ITAIL_FAIL_SLOTS 32
#define ITAIL_FRAME_SCAN 256        /* bytes up the stack: a frame, not a walk */

static uint64_t s_itail_failed;

/* Is the dword at guest address `sp` a plausible return address? The same test
 * the ICALL path applies to a target, because it is the same question. */
static int itail_slot_is_code(uint32_t sp, uint32_t *out)
{
    uint32_t v;
    if (!sp || (sp & 3u) || g_xbox_code_hi == 0u) return 0;
    v = MEM32(sp);
    if (out) *out = v;
    return v >= g_xbox_code_lo && v < g_xbox_code_hi;
}

void recomp_itail_fail_log(uint32_t va)
{
    static uint32_t addr[ITAIL_FAIL_SLOTS];
    static uint64_t hits[ITAIL_FAIL_SLOTS];
    static int used;
    uint32_t sp = g_esp;
    uint32_t at_sp = 0;
    int on_return_address;
    int i;

    /* The unresolved-VA record the runaway banner joins up with. A tail jump
     * that resolved to nothing is as much an unresolved branch as the
     * RECOMP_ICALL case, and it is far more often the one that starts a spin. */
    s_last_unresolved_va = va;
    s_last_unresolved_at = g_icall_count;
    s_unresolved_total++;
    s_itail_failed++;

    for (i = 0; i < used; i++) {
        if (addr[i] == va) break;
    }
    if (i == used) {
        if (used >= ITAIL_FAIL_SLOTS) return;
        addr[used] = va;
        hits[used] = 0;
        used++;
    }
    hits[i]++;
    if (!recomp_icall_log_cadence(hits[i])) return;

    on_return_address = itail_slot_is_code(sp, &at_sp);

    fprintf(stderr,
            "[ITAIL] Unresolved tail jump to 0x%08X (%llu times; %llu unresolved"
            " branches in all)\n"
            "[ITAIL]   guest esp=0x%08X holds 0x%08X, which is %s\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)s_unresolved_total,
            sp, at_sp, on_return_address ? "code" : "NOT CODE");

    if (on_return_address) {
        /* The benign reading, and it does happen: a genuine tail call whose
         * target simply was not translated. esp += 4 is correct, one call is
         * lost, the frame is not. */
        fprintf(stderr,
                "[ITAIL]   The epilogue had run, so this is a genuine tail call to an\n"
                "[ITAIL]   untranslated target: one call lost, stack intact.\n");
    } else {
        uint32_t probe, found = 0;
        unsigned depth = 0;
        for (probe = sp + 4; probe <= sp + ITAIL_FRAME_SCAN; probe += 4) {
            if (itail_slot_is_code(probe, NULL)) { found = probe; break; }
        }
        depth = found ? (unsigned)(found - sp) : 0u;
        fprintf(stderr,
                "[ITAIL]   THE EPILOGUE NEVER RAN. esp is still inside the jumping\n"
                "[ITAIL]   function's frame, so `esp += 4` has just returned its caller\n"
                "[ITAIL]   through a local variable. 0x%08X is almost certainly a switch\n"
                "[ITAIL]   ARM interior to a translated function, not a missing function:\n"
                "[ITAIL]   widen that function's extent. DO NOT seed this address -- that\n"
                "[ITAIL]   truncates its container and corrupts every caller instead.\n",
                va);
        if (found) {
            fprintf(stderr,
                    "[ITAIL]   Nearest plausible return address is at esp+%u (0x%08X), so\n"
                    "[ITAIL]   about %u bytes of frame have just been stranded.\n",
                    depth, found, depth);
        } else {
            fprintf(stderr,
                    "[ITAIL]   No plausible return address within %u bytes above esp: the\n"
                    "[ITAIL]   guest stack is already too far gone to size the frame.\n",
                    (unsigned)ITAIL_FRAME_SCAN);
        }
    }
    fflush(stderr);
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

/* Should a count of `hits` on one address print a line?
 *
 * Powers of ten, all the way up, rather than "every ten thousand after the
 * first thousand". The old cadence is unbounded, and unbounded is not a
 * cadence: a guest spinning on a skipped call reached 10,863,700,000 of them
 * in one run and 23,429,500,000 in another, which is 1.09M and 2.34M lines of
 * the same sentence. The 1.7M-line logs that came out of that afternoon are
 * not a logging nuisance, they are the reason nobody read them.
 *
 * Nothing below 10,000 hits changes, and the corpus says a healthy run never
 * gets there: across 782 runs the skipped calls that were NOT a runaway
 * numbered between one and ten. A run that behaves logs exactly what it
 * logged before.
 *
 * Exposed, not static, because the cadence is the half of this that a test can
 * check without a game. */
int recomp_icall_log_cadence(uint64_t hits)
{
    uint64_t step = 1;
    /* Stop one decade short of the width so the multiply cannot wrap: a
     * wrapped step compares equal to nothing and the line is simply lost. */
    while (step < hits && step <= 1000000000000000000ULL) {
        step *= 10;
    }
    return step == hits;
}

/* How many skipped calls on ONE address before the runtime calls it a runaway.
 * 0 disables the check entirely.
 *
 * The default is the same line the corpus analysis drew: classifying 782 runs
 * by whether the NULL indirect call ran away (>= 1e6 calls) rather than merely
 * occurred separates 17 dead runs from 20 that carried a harmless trickle and
 * finished. Below a million this says nothing; above it, the guest is not
 * coming back. */
uint64_t recomp_icall_runaway_threshold(void)
{
    static uint64_t cached;
    static int resolved;
    if (!resolved) {
        const char *env = getenv("RECOMP_ICALL_RUNAWAY");
        resolved = 1;
        cached = 1000000ULL;
        if (env && *env) {
            char *end = 0;
            unsigned long long v = strtoull(env, &end, 0);
            if (end && *end == 0) cached = (uint64_t)v;
        }
    }
    return cached;
}

/* Whether to stop the process once a runaway is named. OFF by default: naming
 * it costs a healthy run nothing, and ending one is a behaviour change that
 * has to be asked for. exit() rather than abort() so the atexit hook that
 * writes icall_targets.dump still runs -- the whole point of stopping here is
 * to leave the evidence in the file the next regeneration reads. */
int recomp_icall_runaway_aborts(void)
{
    static int cached, resolved;
    if (!resolved) {
        const char *env = getenv("RECOMP_ICALL_RUNAWAY_ABORT");
        resolved = 1;
        cached = (env && *env && *env != '0');
    }
    return cached;
}

void recomp_icall_not_code_log(uint32_t va)
{
    static uint32_t addr[NOT_CODE_SLOTS];
    static uint64_t hits[NOT_CODE_SLOTS];
    static char named[NOT_CODE_SLOTS];
    static int used;
    uint64_t limit;
    int i;

    for (i = 0; i < used; i++) {
        if (addr[i] == va) break;
    }
    if (i == used) {
        if (used >= NOT_CODE_SLOTS) return;   /* bounded; the busy ones are in */
        addr[used] = va;
        hits[used] = 0;
        named[used] = 0;
        used++;
    }
    hits[i]++;

    if (named[i]) return;                     /* already diagnosed; say no more */

    limit = recomp_icall_runaway_threshold();
    if (limit && hits[i] >= limit) {
        int k;
        named[i] = 1;
        fprintf(stderr,
                "[ICALL] RUNAWAY: target 0x%08X skipped %llu times -- the guest is not\n"
                "[ICALL] RUNAWAY:   going to recover. A skipped indirect call returns as if the\n"
                "[ICALL] RUNAWAY:   callee ran, so its caller resumed on registers nothing\n"
                "[ICALL] RUNAWAY:   restored and is now looping on a pointer that is not code.\n",
                va, (unsigned long long)hits[i]);
        if (s_last_unresolved_va) {
            fprintf(stderr,
                    "[ICALL] RUNAWAY:   THE CAUSE IS UPSTREAM. The last guest VA the dispatch\n"
                    "[ICALL] RUNAWAY:   table could not resolve was 0x%08X, at indirect call\n"
                    "[ICALL] RUNAWAY:   #%llu (%llu unresolved branches in total, %llu of them\n"
                    "[ICALL] RUNAWAY:   tail jumps -- see the [ITAIL] lines). Translate it, or, if it\n"
                    "[ICALL] RUNAWAY:   is a switch arm, widen the extent of the function that\n"
                    "[ICALL] RUNAWAY:   dispatches to it. Do NOT seed an address interior to a body.\n",
                    s_last_unresolved_va,
                    (unsigned long long)s_last_unresolved_at,
                    (unsigned long long)s_unresolved_total,
                    (unsigned long long)s_itail_failed);
        } else {
            fprintf(stderr,
                    "[ICALL] RUNAWAY:   No indirect call has failed to resolve in this run, so the\n"
                    "[ICALL] RUNAWAY:   bad pointer came from somewhere else -- an uninitialised\n"
                    "[ICALL] RUNAWAY:   object, or a caller whose stack was already wrong.\n");
        }
        fprintf(stderr, "[ICALL] RUNAWAY:   recent resolved targets:");
        for (k = 0; k < ICALL_TRACE_SIZE; k++) {
            uint32_t t = g_icall_trace[(g_icall_trace_idx - ICALL_TRACE_SIZE + k)
                                       & (ICALL_TRACE_SIZE - 1)];
            if (t) fprintf(stderr, " %08X", t);
        }
        fprintf(stderr, "\n");
        if (recomp_icall_runaway_aborts()) {
            fprintf(stderr, "[ICALL] RUNAWAY:   RECOMP_ICALL_RUNAWAY_ABORT is set; stopping here.\n");
            fflush(stderr);
            exit(70);
        }
        fprintf(stderr,
                "[ICALL] RUNAWAY:   Further skips of this target are silent. Set\n"
                "[ICALL] RUNAWAY:   RECOMP_ICALL_RUNAWAY_ABORT=1 to end the run here instead,\n"
                "[ICALL] RUNAWAY:   or RECOMP_ICALL_RUNAWAY=<n> to move the threshold (0 = off).\n");
        fflush(stderr);
        return;
    }

    if (recomp_icall_log_cadence(hits[i])) {
        fprintf(stderr, "[ICALL] skipped not-code target 0x%08X (%llu times)\n",
                va, (unsigned long long)hits[i]);
        fflush(stderr);
    }
}
