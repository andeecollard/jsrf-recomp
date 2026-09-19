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

static int itail_code_u8(uint32_t va, uint8_t *out)
{
    if (g_xbox_code_hi == 0u || va < g_xbox_code_lo || va >= g_xbox_code_hi)
        return 0;
    *out = MEM8(va);
    return 1;
}

/* Length of an `FF /2` or `FF /3` (call r/m32) starting at `at`, or 0.
 *
 * ModRM and SIB only; no prefixes. A call reached through a segment override
 * or an operand-size prefix does not happen in compiled 32-bit code, and
 * guessing at prefixes would cost the precision this whole function exists to
 * buy. */
static int itail_ff_call_len(uint32_t at)
{
    uint8_t modrm, sib;
    int mod, rm, n;

    if (!itail_code_u8(at + 1, &modrm)) return 0;
    if (((modrm >> 3) & 7) != 2 && ((modrm >> 3) & 7) != 3) return 0;
    mod = modrm >> 6;
    rm = modrm & 7;
    n = 2;                                   /* FF + ModRM */
    sib = 0;
    if (mod != 3 && rm == 4) {               /* SIB present */
        if (!itail_code_u8(at + 2, &sib)) return 0;
        n += 1;
    }
    if (mod == 1) n += 1;                    /* disp8 */
    else if (mod == 2) n += 4;               /* disp32 */
    else if (mod == 0) {
        if (rm == 5) n += 4;                 /* disp32, no base */
        else if (rm == 4 && (sib & 7) == 5) n += 4;
    }
    return n;
}

/* Is `va` the address immediately after a call instruction?
 *
 * THIS IS THE WHOLE CORRECTION. The first version asked only whether a stack
 * slot held a CODE ADDRESS, and a frame is full of code addresses that are not
 * return addresses -- vtable pointers, callbacks, function pointers in locals.
 * On the player's run of 19 Sep 2026 the scan stopped at esp+72 on one of
 * those and reported a 72-byte stranded frame for sub_00114A80, whose frame is
 * 0xA4 = 164 bytes: `sub esp,0x98` plus ebx, ebp and esi, confirmed to the
 * byte by its own `mov eax,[esp+0xa8]` reading the stdcall argument that sits
 * at esp_entry+4, and by the `ret 4` it never reached. The instrument
 * understated the damage by a factor of 2.3 while sounding exact, which is
 * worse than not measuring it.
 *
 * A return address is not merely a code address: it is the address AFTER a
 * call. A function pointer in a local points at a function START, and a
 * function start is not preceded by a call. That one extra question separates
 * them, costs a handful of byte reads, and needs no disassembler. */
static int itail_is_return_site(uint32_t va)
{
    uint8_t op;
    int len;

    if (va < g_xbox_code_lo + 8 || va >= g_xbox_code_hi) return 0;
    if (itail_code_u8(va - 5, &op) && op == 0xE8) return 1;   /* call rel32  */
    if (itail_code_u8(va - 7, &op) && op == 0x9A) return 1;   /* far call    */
    for (len = 2; len <= 7; len++) {                          /* call r/m32  */
        if (!itail_code_u8(va - len, &op) || op != 0xFF) continue;
        if (itail_ff_call_len(va - len) == len) return 1;
    }
    return 0;
}

/* The nearest slot above `sp` that really holds a return address.
 *
 * Returns its guest address, or 0 if none within `limit` bytes. `loose_out`
 * receives the nearest slot holding ANY code address -- the answer the first
 * version gave -- because when the strict scan finds nothing that number is
 * still a floor, and a floor reported as a floor is honest where a floor
 * reported as a measurement is not.
 *
 * Split out from the logger so a test can drive it over a synthetic stack;
 * diagnostics/jsrf_first_fault/icall_runaway_test.c builds exactly the shape
 * that fooled it -- a function pointer nearer than the real return address --
 * and pins that the strict answer steps over it. */
uint32_t recomp_itail_frame_top(uint32_t sp, uint32_t limit, uint32_t *loose_out)
{
    uint32_t probe, value, loose = 0;

    for (probe = sp + 4; probe <= sp + limit; probe += 4) {
        if (!itail_slot_is_code(probe, &value)) continue;
        if (!loose) loose = probe;
        if (itail_is_return_site(value)) {
            if (loose_out) *loose_out = loose;
            return probe;
        }
    }
    if (loose_out) *loose_out = loose;
    return 0;
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
        uint32_t loose = 0;
        uint32_t found = recomp_itail_frame_top(sp, ITAIL_FRAME_SCAN, &loose);
        unsigned depth = found ? (unsigned)(found - sp) : 0u;
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
                    "[ITAIL]   Nearest RETURN SITE (a code address preceded by a call) is at\n"
                    "[ITAIL]   esp+%u (0x%08X), so about %u bytes of frame are stranded.\n",
                    depth, found, depth);
            if (loose && loose < found) {
                /* Say when the cheap test would have answered differently.
                 * That gap is the bug this scan was rewritten for, and seeing
                 * it in a log is how anyone would notice it coming back. */
                fprintf(stderr,
                        "[ITAIL]   (a code pointer sits nearer, at esp+%u -- a local, not a\n"
                        "[ITAIL]   return address; the earlier scan stopped there and"
                        " understated this.)\n",
                        (unsigned)(loose - sp));
            }
        } else if (loose) {
            fprintf(stderr,
                    "[ITAIL]   No return site within %u bytes above esp. AT LEAST %u bytes\n"
                    "[ITAIL]   are stranded -- that is a FLOOR from the nearest code pointer\n"
                    "[ITAIL]   at esp+%u, not a measurement of the frame.\n",
                    (unsigned)ITAIL_FRAME_SCAN, (unsigned)(loose - sp),
                    (unsigned)(loose - sp));
        } else {
            fprintf(stderr,
                    "[ITAIL]   No return site and no code pointer within %u bytes above esp:\n"
                    "[ITAIL]   the guest stack is already too far gone to size the frame.\n",
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
    /* THE RING, SNAPSHOT BEFORE THE SPIN DESTROYS IT.
     *
     * The banner used to print g_icall_trace live and, on the player's run of
     * 19 Sep 2026, printed nothing at all: "recent resolved targets:" followed
     * by an empty line. Of course it did. RECOMP_ICALL writes every target
     * into that ring including the bad one, and the spin calls the bad one
     * millions of times, so sixteen iterations in the entire ring is zeros and
     * the `if (t)` filter drops all of it. The one piece of context the banner
     * exists to carry is erased by the very event it describes.
     *
     * Take the copy on the FIRST sighting of an address, when the ring still
     * holds what the guest was doing before it went wrong. */
    static uint32_t before[NOT_CODE_SLOTS][ICALL_TRACE_SIZE];
    static int used;
    uint64_t limit, total;
    int i, k;

    for (i = 0; i < used; i++) {
        if (addr[i] == va) break;
    }
    if (i == used) {
        if (used >= NOT_CODE_SLOTS) return;   /* bounded; the busy ones are in */
        addr[used] = va;
        hits[used] = 0;
        named[used] = 0;
        for (k = 0; k < ICALL_TRACE_SIZE; k++) {
            before[used][k] =
                g_icall_trace[(g_icall_trace_idx - ICALL_TRACE_SIZE + k)
                              & (ICALL_TRACE_SIZE - 1)];
        }
        used++;
    }
    hits[i]++;

    if (named[i]) return;                     /* already diagnosed; say no more */

    limit = recomp_icall_runaway_threshold();
    if (limit && hits[i] >= limit) {
        named[i] = 1;
        /* SUM ACROSS SLOTS, AND STAY QUIET IF ANOTHER ALREADY SPOKE.
         *
         * The find-or-add above is not atomic, so two guest threads spinning
         * on the same target can each end up with their own slot. That is
         * exactly what the player's run did: the banner printed TWICE, both
         * copies reading "skipped 1000000 times", because each slot counted
         * its own million. The message was duplicated and the total halved.
         *
         * Locking the fast path of a diagnostic that runs a million times in
         * a spin is the wrong trade, so leave the race and repair the
         * REPORT: add up every slot holding this address, and if one of them
         * has already been named, this thread has nothing to add. */
        total = 0;
        for (k = 0; k < used; k++) {
            if (addr[k] != va) continue;
            total += hits[k];
            if (k != i && named[k]) return;
        }
        fprintf(stderr,
                "[ICALL] RUNAWAY: target 0x%08X skipped %llu times -- the guest is not\n"
                "[ICALL] RUNAWAY:   going to recover. A skipped indirect call returns as if the\n"
                "[ICALL] RUNAWAY:   callee ran, so its caller resumed on registers nothing\n"
                "[ICALL] RUNAWAY:   restored and is now looping on a pointer that is not code.\n",
                va, (unsigned long long)total);
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
        /* The ring AS IT WAS when this target first appeared. Printing it
         * live shows sixteen copies of the bad target and nothing else. */
        fprintf(stderr, "[ICALL] RUNAWAY:   targets in flight when it first"
                        " appeared:");
        for (k = 0; k < ICALL_TRACE_SIZE; k++) {
            if (before[i][k]) fprintf(stderr, " %08X", before[i][k]);
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

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. This is the
 * runtime half: say the site was REACHED, with what state, and how often --
 * because the translator's tally lists 122 such sites in JSRF and cannot tell
 * a dead one (a linear sweep reading `bound` or `arpl` over data) from a live
 * one (Wreckless's `bsf`, which was live, and cost a bring-up).
 *
 * The instruction is still a no-op. This changes no behaviour; it only stops
 * the omission being silent. RECOMP_UNIMPL_TRAP=1 aborts at the first hit,
 * which is the validation-build mode: stop at the cause, not downstream.
 *
 * Cadence is the ICALL one -- powers of ten per site -- so a live site in a
 * hot loop prints about twenty lines a run and a healthy run prints nothing,
 * because a healthy run reaches none of these. */
#include "../../src/recomp_switch.h"

#define UNIMPL_SLOTS 64

static uint64_t s_unimpl_total;

void recomp_unimpl(const char *text, uint32_t va)
{
    static uint32_t addr[UNIMPL_SLOTS];
    static uint64_t hits[UNIMPL_SLOTS];
    static int used;
    static int trap = -1;
    uint64_t n;
    int i;

    if (trap < 0) trap = recomp_switch_on("RECOMP_UNIMPL_TRAP");
    s_unimpl_total++;

    for (i = 0; i < used; i++) {
        if (addr[i] == va) break;
    }
    if (i == used) {
        if (used >= UNIMPL_SLOTS) {
            /* More distinct live sites than slots: not tracked per site, but
             * never silent. Print the overflow once, and still honour the
             * trap. */
            static int overflow_said;
            if (!overflow_said) {
                overflow_said = 1;
                fprintf(stderr, "[UNIMPL] more than %d distinct untranslated"
                        " sites reached; further ones are counted but not"
                        " listed\n", UNIMPL_SLOTS);
            }
            if (!trap) return;
            n = 1;
        } else {
            addr[used] = va;
            hits[used] = 0;
            used++;
            n = ++hits[i];
        }
    } else {
        n = ++hits[i];
    }

    if (trap || recomp_icall_log_cadence(n)) {
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (%llu times at this site; %llu untranslated hits in all)\n"
                "[UNIMPL]   eax=%08X ecx=%08X edx=%08X ebx=%08X"
                " esi=%08X edi=%08X seh_ebp=%08X esp=%08X\n"
                "[UNIMPL]   it was a no-op: the guest continues with the state"
                " above. RECOMP_UNIMPL_TRAP=1 stops here instead.\n",
                text, va, (unsigned long long)n,
                (unsigned long long)s_unimpl_total,
                g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp, g_esp);
        fflush(stderr);
    }
    if (trap) {
        fprintf(stderr, "[UNIMPL] RECOMP_UNIMPL_TRAP is on: stopping at the"
                " first untranslated instruction, at its own address.\n");
        fflush(stderr);
        abort();
    }
}
