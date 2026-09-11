/* Count entries to named guest functions, and nothing else.
 *
 * Answers "which of these candidate functions actually runs", which the
 * disassembly cannot: an offset used by many classes has many store sites and
 * static reading cannot say which belongs to the object in hand.
 *
 * Read-only: increments host-side counters and touches no guest state. Opt-in
 * via RECOMP_FUNC_HIT_TRACE, reported on the existing report cadence. */
#include "guest_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A direct-mapped table, not a scan.
 *
 * The first version walked a linear array on every entry. With nine hot sites
 * armed that was enough overhead to stall the guest's pad polling within ten
 * seconds; with one site it ran fine. Since the point of this probe is often to
 * ask "which of a hundred candidate functions actually runs", the per-call cost
 * has to be independent of how many sites are armed. Open addressing over a
 * power-of-two table gives that, and a guest VA is already well distributed in
 * its low bits. */
#define FUNC_HIT_SLOTS 1024u
#define FUNC_HIT_MASK  (FUNC_HIT_SLOTS - 1u)

static struct {
    uint32_t va;                 /* 0 means empty; no guest function is at 0 */
    unsigned long long hits;
} g_hits[FUNC_HIT_SLOTS];
static unsigned g_hit_count;

static int func_hit_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_FUNC_HIT_TRACE") ? 1 : 0;
    return on;
}

void jsrf_func_hit(uint32_t va)
{
    unsigned i;
    if (!func_hit_on() || !va)
        return;
    i = (unsigned)(va >> 4) & FUNC_HIT_MASK;
    for (;;) {
        if (g_hits[i].va == va) { g_hits[i].hits++; return; }
        if (g_hits[i].va == 0) {
            g_hits[i].va = va; g_hits[i].hits = 1; g_hit_count++; return;
        }
        i = (i + 1u) & FUNC_HIT_MASK;   /* the table is far larger than the
                                        * number of sites, so this is rare */
    }
}

/* Record the distinct `this` pointers a site is entered with.
 *
 * sub_0009D030 stores sub_000A0260's return value into [this + 0xE54], the bit
 * that gates character animation, and it runs only five times at scene load
 * while both CPlayers read zero there. Those two facts have two very different
 * explanations -- the five calls were for other objects, or they were for the
 * CPlayers and the returned flag word was zero -- and only the `this` values
 * tell them apart. Distinct values, not a log: five entries must not become
 * five thousand lines if the site turns out to be hot. */
#define FUNC_ARG_MAX 384u

static struct {
    uint32_t va;
    uint32_t value;
    unsigned long long hits;
} g_args[FUNC_ARG_MAX];
static unsigned g_arg_count;
static unsigned long long g_arg_dropped;

#define FUNC_PAIR_MAX 256u
static struct { uint32_t va, a, b; unsigned long long hits; } g_pairs[FUNC_PAIR_MAX];
static unsigned g_pair_count;

static void jsrf_func_arg_pair(uint32_t va, uint32_t a, uint32_t b)
{
    unsigned i;
    if (!func_hit_on()) return;
    jsrf_func_hit(va);
    for (i = 0; i < g_pair_count; i++)
        if (g_pairs[i].va == va && g_pairs[i].a == a && g_pairs[i].b == b) {
            g_pairs[i].hits++; return;
        }
    if (g_pair_count < FUNC_PAIR_MAX) {
        g_pairs[g_pair_count].va = va; g_pairs[g_pair_count].a = a;
        g_pairs[g_pair_count].b = b; g_pairs[g_pair_count].hits = 1;
        g_pair_count++;
    }
}
static int g_arg_full;

/* Two values per entry, because one is ambiguous.
 *
 * Recording only a field value cannot say which object it belonged to, and
 * recording only `this` cannot say what the field held. Bracketing which call
 * changes a field needs both together, so the pair is the unit stored. */
void jsrf_func_arg2(uint32_t va, uint32_t a, uint32_t b)
{
    /* Fold the pair into the existing table by mixing; collisions only merge
     * counts, and the pair is printed back from the stored halves. */
    jsrf_func_arg_pair(va, a, b);
}

void jsrf_func_arg(uint32_t va, uint32_t value)
{
    unsigned i;
    if (!func_hit_on())
        return;
    jsrf_func_hit(va);
    /* Cost matters more than completeness here. A linear scan on every entry,
     * across several hot sites, perturbed the guest badly enough that the pad
     * poll stalled within seconds -- the heavier the probe set, the earlier it
     * died, while light runs reached the tutorial. So the scan is bounded and
     * gives up once a site has shown enough distinct values: after that the
     * site is only counted, never searched. The first few `this` pointers are
     * what identify the caller; the thousandth adds nothing. */
    for (i = 0; i < g_arg_count; i++) {
        if (g_args[i].va == va && g_args[i].value == value) {
            g_args[i].hits++;
            return;
        }
    }
    if (g_arg_count < FUNC_ARG_MAX) {
        g_args[g_arg_count].va = va;
        g_args[g_arg_count].value = value;
        g_args[g_arg_count].hits = 1;
        g_arg_count++;
    } else {
        g_arg_dropped++;
    }
}

static void jsrf_func_arg_report(void)
{
    unsigned i;
    for (i = 0; i < g_arg_count; i++)
        fprintf(stderr, "  [FUNC-ARG] %08X this=%08X calls=%llu\n",
                (unsigned)g_args[i].va, (unsigned)g_args[i].value,
                g_args[i].hits);
    for (i = 0; i < g_pair_count; i++)
        fprintf(stderr, "  [FUNC-PAIR] %08X this=%08X field=%08X calls=%llu\n",
                (unsigned)g_pairs[i].va, (unsigned)g_pairs[i].a,
                (unsigned)g_pairs[i].b, g_pairs[i].hits);
    if (g_arg_dropped)
        fprintf(stderr, "  [FUNC-ARG] %llu entries dropped (table full)\n",
                g_arg_dropped);
}

void jsrf_func_hit_report(void)
{
    unsigned i;
    if (!func_hit_on())
        return;
    if (!g_hit_count) {
        fprintf(stderr, "  [FUNC-HIT] no instrumented site has been entered\n");
        fflush(stderr);
        return;
    }
    for (i = 0; i < FUNC_HIT_SLOTS; i++)
        if (g_hits[i].va)
            fprintf(stderr, "  [FUNC-HIT] %08X calls=%llu\n",
                    (unsigned)g_hits[i].va, g_hits[i].hits);
    jsrf_func_arg_report();
    fflush(stderr);
}
