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

/* The ORDER the sites were entered in, not just how often.
 *
 * Counting brackets a divergence to a SET of functions; it cannot say which
 * ran when, and "how did the guest get here" is an ordering question.
 *
 * The ring is CIRCULAR and per thread. It was head-capped first, which was
 * wrong: both hosts are identical for hundreds of steps and what differs is
 * where a thread STOPS, which a ring that fills up and then ignores everything
 * afterwards cannot show.
 *
 * KNOWN LIMIT, and it blocks cross-host use. The slot a thread gets is handed
 * out by FIRST-TOUCH ORDER, so slot N is a different thread on each host and
 * on each RUN -- macOS slot 0 opened with 00192A80 in one run and 00154420 in
 * the next. Comparing "thread 0" against "thread 0" across hosts is therefore
 * meaningless, and matching threads by common prefix fails too once the runs
 * sit at different stages: measured, no pair across the two hosts shared even
 * three collapsed steps. To use this differentially the ring has to be keyed
 * by a STABLE identity -- the first guest function a thread enters, or a role
 * assigned where the thread is created -- not by the order slots are claimed.
 * As it stands this answers "where did this thread stop" on ONE host.
 *
 * Two things the DIFF needs, which are not obvious and cost a wrong answer
 * each: collapse consecutive repeats first, or a poll that takes a different
 * number of spins offsets everything after it; and filter the ISR entry points
 * if the rings are merged, because an interrupt taken at a different moment is
 * scheduling, not divergence. */
#define FUNC_SEQ_MAX 1024u
#define FUNC_SEQ_THREADS 8u

/* PER THREAD, deliberately. A single global ring interleaves the guest's main
 * thread with the ISRs and the DPCs they dispatch, and a flat interleaved log
 * cannot be diffed against another host: whichever host happens to take an
 * interrupt at that moment shows a "divergence" that is only scheduling. That
 * mistake was made three times in one session -- 0x00193C50 (the vblank ISR)
 * and 0x00194480 (D3D's vblank DPC) both read as guest-path divergences before
 * anyone noticed they run on another context. Hand-filtering known entry
 * points does not scale; separating the threads does. */
typedef struct {
    uint32_t seq[FUNC_SEQ_MAX];
    unsigned n;
    unsigned id;
    uint32_t first;   /* the first armed site this thread entered -- a STABLE
                       * name for it, unlike the slot index, so the same
                       * logical thread can be found on both hosts. */
} FuncSeq;
static FuncSeq g_seqs[FUNC_SEQ_THREADS];
static volatile int g_seq_threads;
static _Thread_local FuncSeq *g_my_seq;

/* Fire a callback the moment a site's entry count reaches a value, ON THE
 * THREAD THAT GOT THERE.
 *
 * Polling the count from somewhere else -- the pushbuffer ack loop, say --
 * pins the FRAME but not the position within it, because the poller is a
 * different thread from the guest's main loop. Measured: two runs of the SAME
 * host, anchored the same way, disagreed on id 7's +0x44 and on id 7665's
 * zsort, and a Windows-vs-macOS comparison at the same anchor disagreed on
 * exactly the same fields. That is intra-frame phase, and it is indistinguish-
 * able from a real host divergence unless a same-host control is run at every
 * anchor. Firing here instead puts both hosts at the same instruction
 * boundary. */
static uint32_t g_alarm_va;
static unsigned long long g_alarm_count;
static void (*g_alarm_fn)(void);

void jsrf_func_hit_alarm(uint32_t va, unsigned long long count, void (*fn)(void))
{
    g_alarm_va = va; g_alarm_count = count; g_alarm_fn = fn;
}

static void func_hit_alarm(uint32_t va, unsigned long long hits)
{
    void (*fn)(void) = g_alarm_fn;
    if (!fn || va != g_alarm_va || hits != g_alarm_count) return;
    g_alarm_fn = NULL;            /* once */
    fn();
}

void jsrf_func_hit(uint32_t va)
{
    unsigned i;
    if (!func_hit_on() || !va)
        return;
    if (!g_my_seq) {
        int slot = __atomic_fetch_add(&g_seq_threads, 1, __ATOMIC_SEQ_CST);
        if (slot >= 0 && slot < (int)FUNC_SEQ_THREADS) {
            g_my_seq = &g_seqs[slot];
            g_my_seq->id = (unsigned)slot;
        }
    }
    /* CIRCULAR, not head-capped. The head is the same on both hosts for
     * hundreds of steps -- what differs is where a thread STOPS, and a ring
     * that fills up and then ignores everything afterwards cannot show that. */
    if (g_my_seq) {
        if (!g_my_seq->first) g_my_seq->first = va;
        g_my_seq->seq[g_my_seq->n++ % FUNC_SEQ_MAX] = va;
    }
    i = (unsigned)(va >> 4) & FUNC_HIT_MASK;
    for (;;) {
        if (g_hits[i].va == va) {
            unsigned long long n = ++g_hits[i].hits;
            func_hit_alarm(va, n);
            return;
        }
        if (g_hits[i].va == 0) {
            g_hits[i].va = va; g_hits[i].hits = 1; g_hit_count++;
            func_hit_alarm(va, 1);
            return;
        }
        i = (i + 1u) & FUNC_HIT_MASK;   /* the table is far larger than the
                                        * number of sites, so this is rare */
    }
}

/* How many times an armed site has been entered, for anyone who needs the
 * GUEST's own clock rather than the host's.
 *
 * The two hosts run at very different speeds -- Windows about a fifth of macOS
 * on the main loop -- so anything anchored to wall-clock compares two different
 * moments in the guest's life and calls the difference a divergence. This
 * project has already published three handovers that made exactly that mistake.
 * Anchoring to a site's entry count instead compares the same instant of guest
 * execution on both hosts, whatever the hosts were doing.
 *
 * Returns 0 for a site that is not armed, which is indistinguishable from one
 * that has not run yet -- so callers that care must arm the site deliberately
 * and check it is counting before trusting a zero. */
unsigned long long jsrf_func_hit_count(uint32_t va)
{
    unsigned i = (unsigned)(va >> 4) & FUNC_HIT_MASK;
    unsigned probes;
    if (!va) return 0;
    for (probes = 0; probes <= FUNC_HIT_MASK; ++probes) {
        if (g_hits[i].va == va) return g_hits[i].hits;
        if (g_hits[i].va == 0)  return 0;
        i = (i + 1u) & FUNC_HIT_MASK;
    }
    return 0;
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

/* One object's exec dispatch, edge-triggered.
 *
 * CActBase::recursiveExec0Default walks the scene tree and calls each live
 * object's exec virtual -- vtable slot 1 -- from a single site at 0x00011083,
 * with the object in ecx. That is the cheapest place in the frame to ask
 * whether a particular object is still being updated: one site, every object,
 * once per frame.
 *
 * Two ids, not one. 45 is Corn, the object whose animation has stalled; 44 is
 * the other CPlayer in the tutorial and is STATIC BY DESIGN -- xemu changes two
 * dwords on it in 10 s and the same two in 30 s. Carrying it through the same
 * run makes the control part of the measurement: if both read zero the probe is
 * dead, and only if 44 behaves while 45 does not is the asymmetry real. An
 * absence with no positive control has already cost this effort a cycle.
 *
 * Edge-triggered, because the site runs ~14,700 times per report interval and
 * a line per call is both useless and fatal: the last attempt at this used
 * jsrf_func_arg, whose linear scan over 384 entries at two hot sites perturbed
 * the guest enough that the title screen stopped responding. Here the filter is
 * one load and a compare in the generated C, and this function is only entered
 * for the two objects that matter.
 */
struct corn_seen {
    uint32_t id, self, flags, state11c, exec;
    /* The CPlayer's own state machine, read from the same hook.
     * sub_00080340 -- the exec virtual both CPlayers dispatch -- opens on
     *     eax = [this+0xE50]; if (eax < 0x14) ...; if (eax <= 0x1A) skip
     *     eax = [this+0xE60]; [this+0xE68] = 0
     *     if (eax > 0) { [this+0xE60] = --eax; if (!eax) [this+0xE68] = 1; }
     * so +0xE50 is the state index, +0xE60 a countdown and +0xE68 its expiry
     * flag. If the state is frozen, those three say whether the timer is even
     * running -- which +0x11C alone cannot. */
    uint32_t e50, e60, e68;
    unsigned long long calls;
    int primed;
};
static struct corn_seen g_corn[2];

/* Every object dispatched from 0x00011083, not just the two. Incremented
 * inline in the generated C so the walker's own liveness comes from the same
 * hook: "Corn is not updated" and "nothing is updated" are different faults
 * and an id-filtered counter cannot tell them apart. */
unsigned long long g_exec_dispatches;

double xbox_InputSeconds(void);

void jsrf_corn_note(uint32_t id, uint32_t self, uint32_t flags,
                    uint32_t state11c, uint32_t exec,
                    uint32_t e50, uint32_t e60, uint32_t e68)
{
    struct corn_seen *c;

    if (!func_hit_on()) return;
    if (id != 44u && id != 45u) return;
    /* Also bump the ordinary counter for this site. jsrf_func_hit_report bails
     * with "no instrumented site has been entered" when g_hit_count is zero,
     * which silently swallowed the periodic counts on the first run -- and the
     * periodic count is the whole difference between "exec stopped being
     * called" and "exec is called but nothing changes". */
    jsrf_func_hit(0x00011083u);
    c = &g_corn[id - 44u];
    c->calls++;
    if (c->primed && c->self == self && c->flags == flags
        && c->state11c == state11c && c->exec == exec
        && c->e50 == e50 && c->e60 == e60 && c->e68 == e68)
        return;
    fprintf(stderr, "  [CORN] t=%8.2f id=%u this=%08X s11c=%08X"
            " e50=%u e60=%u e68=%u exec=%08X calls=%llu\n",
            xbox_InputSeconds(), (unsigned)id, (unsigned)self,
            (unsigned)state11c, (unsigned)e50, (unsigned)e60, (unsigned)e68,
            (unsigned)exec, c->calls);
    fflush(stderr);
    c->primed = 1; c->self = self; c->flags = flags;
    c->state11c = state11c; c->exec = exec;
    c->e50 = e50; c->e60 = e60; c->e68 = e68;
}

/* Call counts on the report cadence, so "is it still being updated at all"
 * is answerable without a line per frame. */
static void jsrf_corn_report(void)
{
    unsigned i;
    fprintf(stderr, "  [CORN] exec dispatches (all objects)=%llu\n",
            g_exec_dispatches);
    for (i = 0; i < 2; i++)
        if (g_corn[i].primed)
            fprintf(stderr, "  [CORN] id=%u exec_dispatches=%llu"
                    " s11c=%08X e50=%u e60=%u e68=%u\n",
                    44u + i, g_corn[i].calls, (unsigned)g_corn[i].state11c,
                    (unsigned)g_corn[i].e50, (unsigned)g_corn[i].e60,
                    (unsigned)g_corn[i].e68);
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
    {
        unsigned t, n;
        int used = g_seq_threads;
        if (used > (int)FUNC_SEQ_THREADS) used = (int)FUNC_SEQ_THREADS;
        for (t = 0; t < (unsigned)used; t++) {
            FuncSeq *q = &g_seqs[t];
            if (!q->n) continue;
            unsigned total = q->n;
            unsigned shown = total < FUNC_SEQ_MAX ? total : FUNC_SEQ_MAX;
            fprintf(stderr, "  [FUNC-SEQ] thread %u first=%08X: %u entries,"
                    " last %u in call order:\n",
                    t, (unsigned)q->first, total, shown);
            for (n = 0; n < shown; n++) {
                unsigned idx = (total - shown + n) % FUNC_SEQ_MAX;
                fprintf(stderr, "%s%08X%s", (n % 8u) ? " " : "    ",
                        (unsigned)q->seq[idx], (n % 8u) == 7u ? "\n" : "");
            }
            if (shown % 8u) fprintf(stderr, "\n");
        }
    }
    jsrf_func_arg_report();
    jsrf_corn_report();
    fflush(stderr);
}
