/* The ADX guard. See adx_guard.h for why it spans the guest critical section
 * rather than the two function bodies. */

#include "adx_guard.h"
#include "../../src/recomp_switch.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>

/* One thread's standing in the guard. `depth` is this thread's nesting, `id` a
 * small nonzero number that identifies it in the report -- pthread_t is not
 * portably printable and we only ever need to compare it with the recorded
 * owner. */
static __thread unsigned      t_depth;
static __thread unsigned long t_id;

static pthread_mutex_t g_m  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static unsigned long g_owner;        /* t_id of the holder; 0 when free */
/* THE HOLDER BY NAME, NOT BY COUNTER.
 *
 * "holder 2" is an internal sequence number and identifies nothing. Sessions
 * 11 and 12 of 21 Sep 2026 both froze with a holder that never released, and
 * working out WHICH thread that was took a `sample` and an inference. This is
 * the id `sample` itself prints, so the next freeze names it directly. */
static unsigned long long g_owner_tid;

static unsigned long long adx_self_tid(void)
{
#ifdef __APPLE__
    unsigned long long t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
#else
    return (unsigned long long)(uintptr_t)pthread_self();
#endif
}
static unsigned      g_depth;        /* the holder's nesting */
static unsigned long g_next_id = 1;  /* hands out t_id under g_m */

static struct adx_guard_stats g_st;

/* LIVE NESTING, for the trace's leak test: the sum of every thread's t_depth
 * plus what blocking waits have parked (adx_guard_block_begin). Every lock
 * adds one level and every matched unlock removes one, so
 * locks - unlocks_matched - (g_tsum + g_parked) is zero unless a level went
 * missing -- which is the G66 lock leak. Maintained under g_m. */
static long g_tsum;
static long g_parked;
static void prio_report(void);         /* the ledger's report line, below */

int adx_guard_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_ADX_SERIALIZE");
    return on;
}

/* Generous on purpose. This is not a latency knob: any wait long enough to hit
 * it is already a defect, and the only question is whether the run reports it
 * or hangs. */
unsigned adx_guard_timeout_ms(void)
{
    static unsigned ms;
    static int read;
    if (!read) {
        const char *v = getenv("RECOMP_ADX_LOCK_TIMEOUT_MS");
        long n = (v && *v) ? strtol(v, NULL, 10) : 0;
        ms = (n > 0) ? (unsigned)n : 5000u;
        read = 1;
    }
    return ms;
}

/* OFF by default. See adx_guard.h: the steal was measured to cause the wedge
 * it was meant to diagnose, and never measured to rescue one. */
int adx_guard_steal_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_ADX_LOCK_STEAL");
    return on;
}

static unsigned long my_id(void)
{
    /* Called with g_m held, which is what makes the counter safe. */
    if (!t_id) t_id = g_next_id++;
    return t_id;
}

/* Take one level of the guard. Recursion is free; a first acquisition waits
 * for the current holder, and gives up waiting rather than wedging the run. */
static void guard_acquire(void)
{
    pthread_mutex_lock(&g_m);
    {
        unsigned long me = my_id();

        if (t_depth > 0 && g_owner == me) {
            ++t_depth; ++g_tsum;
            g_depth = t_depth;
        } else {
            if (g_owner != 0 && g_owner != me) {
                struct timeval now;
                struct timespec deadline;
                unsigned ms = adx_guard_timeout_ms();
                int timed_out = 0;

                ++g_st.contended;
                gettimeofday(&now, NULL);
                deadline.tv_sec  = now.tv_sec + (time_t)(ms / 1000u);
                deadline.tv_nsec = now.tv_usec * 1000L
                                 + (long)(ms % 1000u) * 1000000L;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec  += 1;
                    deadline.tv_nsec -= 1000000000L;
                }
                while (g_owner != 0 && g_owner != me) {
                    if (pthread_cond_timedwait(&g_cv, &g_m, &deadline) != 0) {
                        if (adx_guard_steal_on()) {
                            timed_out = 1;
                            break;
                        }
                        /* Do NOT take it. Entering while the holder is inside
                         * is what poisons the guest's one saved-priority
                         * slot, and a poisoned run is worse than a stalled
                         * one: it corrupts quietly and then crawls. Say who
                         * we are waiting for and go back to waiting. */
                        ++g_st.stalls;
                        fprintf(stderr,
                                "  [ADX-GUARD] STALLED %u ms: waiter %lu"
                                " (thread %llu) wants the region, holder %lu"
                                " (THREAD %llu) is still inside (stall #%lu)."
                                " Match that thread id against `sample` to"
                                " name it. NOT stealing -- that is what"
                                " poisons 0x0027D0F8.\n",
                                ms, me, adx_self_tid(), g_owner,
                                g_owner_tid, g_st.stalls);
                        fflush(stderr);
                        gettimeofday(&now, NULL);
                        deadline.tv_sec  = now.tv_sec + (time_t)(ms / 1000u);
                        deadline.tv_nsec = now.tv_usec * 1000L
                                         + (long)(ms % 1000u) * 1000000L;
                        if (deadline.tv_nsec >= 1000000000L) {
                            deadline.tv_sec  += 1;
                            deadline.tv_nsec -= 1000000000L;
                        }
                    }
                }
                if (timed_out && g_owner != 0 && g_owner != me) {
                    /* Take it anyway. The previous holder keeps its own
                     * t_depth and will find, on release, that it is no longer
                     * the recorded owner -- see guard_release. */
                    ++g_st.steals;
                }
            }
            g_owner = me;
            g_owner_tid = adx_self_tid();
            ++t_depth; ++g_tsum; /* not "= 1": a steal victim may re-enter */
            g_depth = t_depth;
        }
        if (t_depth > g_st.max_depth) g_st.max_depth = t_depth;
        g_st.depth = g_depth;
        g_st.owner = g_owner;
    }
    pthread_mutex_unlock(&g_m);
}

/* Take one level WITHOUT waiting and WITHOUT stealing: 1 if it is now ours
 * (it was free, or already ours), 0 if another thread is inside the region.
 * This is the unmatched unlock's only way in -- see adx_guard.h. */
static int guard_try_acquire(void)
{
    int got;
    pthread_mutex_lock(&g_m);
    {
        unsigned long me = my_id();

        if (t_depth > 0 && g_owner == me) {
            ++t_depth; ++g_tsum;
            g_depth = t_depth;
            got = 1;
        } else if (g_owner == 0) {
            g_owner = me;
            g_owner_tid = adx_self_tid();
            ++t_depth; ++g_tsum;
            g_depth = t_depth;
            got = 1;
        } else {
            got = 0;
        }
        if (got) {
            if (t_depth > g_st.max_depth) g_st.max_depth = t_depth;
            g_st.depth = g_depth;
            g_st.owner = g_owner;
        }
    }
    pthread_mutex_unlock(&g_m);
    return got;
}

/* Give one level back. A thread that was stolen from drops its own nesting and
 * touches nothing global: the guard belongs to somebody else now. */
static void guard_release(void)
{
    pthread_mutex_lock(&g_m);
    if (t_depth > 0) {
        --t_depth; --g_tsum;
        if (g_owner == t_id) {
            if (g_depth > 0) --g_depth;
            if (g_depth == 0) {
                g_owner = 0;
                pthread_cond_broadcast(&g_cv);
            }
        } else {
            ++g_st.stolen_from;
        }
    }
    g_st.depth = g_depth;
    g_st.owner = g_owner;
    pthread_mutex_unlock(&g_m);
}

/* See adx_guard.h: a holder that blocks in a kernel wait stops excluding. */
unsigned adx_guard_block_begin(void)
{
    unsigned saved = 0;
    pthread_mutex_lock(&g_m);
    if (t_depth > 0 && g_owner == my_id()) {
        saved = t_depth;
        g_tsum -= t_depth;
        g_parked += saved;
        t_depth = 0;
        g_depth = 0;
        g_owner = 0;
        g_owner_tid = 0;
        ++g_st.block_releases;
        g_st.depth = 0;
        g_st.owner = 0;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_m);
    if (saved && adx_trace_on())
        adx_trace_note(ADX_EV_BLOCK_BEGIN, 0, ADX_COUNT_NA, ADX_COUNT_NA, 0,
                       ADX_PRIO_NONE);
    return saved;
}

void adx_guard_block_end(unsigned saved_depth)
{
    int contended;
    if (!saved_depth) return;
    pthread_mutex_lock(&g_m);
    contended = g_owner != 0 && g_owner != my_id();
    if (contended) ++g_st.block_reacquires_contended;
    pthread_mutex_unlock(&g_m);
    guard_acquire();                       /* one level, waiting like any entrant */
    pthread_mutex_lock(&g_m);
    /* Anything taken DURING the wait (a DPC run from the wait loop that locked
     * and has not unlocked) is overwritten here and lost from t_depth; the sum
     * follows t_depth faithfully so the trace's leak test sees it. */
    g_tsum += (long)saved_depth - (long)t_depth;
    g_parked -= saved_depth;
    t_depth = saved_depth;                 /* the nesting the wait interrupted */
    g_depth = saved_depth;
    if (t_depth > g_st.max_depth) g_st.max_depth = t_depth;
    g_st.depth = g_depth;
    pthread_mutex_unlock(&g_m);
    if (adx_trace_on())
        adx_trace_note(ADX_EV_BLOCK_END, 0, ADX_COUNT_NA, ADX_COUNT_NA, 0,
                       ADX_PRIO_NONE);
}

void adx_guard_lock_enter(void)
{
    if (!adx_guard_on()) return;
    guard_acquire();
    pthread_mutex_lock(&g_m);
    ++g_st.locks;
    pthread_mutex_unlock(&g_m);
}

int adx_guard_unlock_enter(void)
{
    if (!adx_guard_on()) return 0;

    /* t_depth is this thread's own, so reading it unlocked is safe: nobody
     * else writes it. A thread that was stolen from still reads > 0 here and
     * is still a matched unlock -- guard_release() sorts that out and counts
     * it as stolen_from. */
    if (t_depth > 0) {
        pthread_mutex_lock(&g_m);
        ++g_st.unlocks_matched;
        pthread_mutex_unlock(&g_m);
        return 1;
    }

    /* Unmatched: sub_001437B0's I/O guard calling the registered unlock on a
     * spin pass. Take the guard only if it is FREE. Waiting here and then
     * stealing is what poisoned 0x0027D0F8 on 21 Sep 2026 -- the body drove
     * the refcount to zero underneath a holder that was still elevated. On
     * hardware this pass could not have run at all, so when another thread is
     * inside we refuse the body rather than serialise it late. */
    if (!guard_try_acquire()) {
        pthread_mutex_lock(&g_m);
        ++g_st.unlocks_skipped;
        pthread_mutex_unlock(&g_m);
        return -1;
    }

    pthread_mutex_lock(&g_m);
    ++g_st.unlocks_unmatched;
    pthread_mutex_unlock(&g_m);
    return 0;
}

void adx_guard_unlock_leave(void)
{
    if (!adx_guard_on()) return;
    guard_release();
}

void adx_guard_read_stats(struct adx_guard_stats *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_m);
    *out = g_st;
    pthread_mutex_unlock(&g_m);
}

/* Named state, per recomp_switch.h: a report that does not say whether the
 * switch was read cannot be used as an A/B arm. */
void adx_guard_report(void)
{
    struct adx_guard_stats s;
    adx_guard_read_stats(&s);
    fprintf(stderr,
            "  [ADX-GUARD] serialize %s: locks=%lu unlocks=%lu matched"
            " (+%lu UNMATCHED, %lu SKIPPED) contended=%lu max_depth=%u"
            " held=%u\n",
            adx_guard_on() ? "on" : "OFF",
            s.locks, s.unlocks_matched, s.unlocks_unmatched,
            s.unlocks_skipped, s.contended, s.max_depth, s.depth);
    fprintf(stderr, "  [ADX-GUARD] released while the holder blocked in a kernel wait: %lu"
                    " (re-taken after waiting for another entrant: %lu)\n",
            s.block_releases, s.block_reacquires_contended);
    if (s.depth)
        fprintf(stderr, "  [ADX-GUARD] held right now by t_id %lu, thread %llu"
                        " -- if this is the same thread every report, it is"
                        " not making progress\n", s.owner, g_owner_tid);
    /* Skipped passes are the fix working, not a fault: each one is a spin
     * pass that hardware would not have run either. They are printed because
     * a number that only ever appears when something is wrong teaches nobody
     * what its healthy value looks like. */
    /* stolen_from rides on the same line as steals rather than a line of its
     * own: it is the other half of one event, and a counter that is collected
     * and never printed is one this tree has had to retire before. */
    if (s.stalls)
        fprintf(stderr,
                "  [ADX-GUARD] %lu STALLS of %u ms waiting for a holder that"
                " was still inside -- waited rather than stole, so the region"
                " stayed serialised and 0x0027D0F8 was not rewritten\n",
                s.stalls, adx_guard_timeout_ms());
    if (s.steals || s.stolen_from)
        fprintf(stderr,
                "  [ADX-GUARD] %lu LOCK-PATH STEALS after %u ms"
                " (RECOMP_ADX_LOCK_STEAL is ON), %lu releases by a thread"
                " already stolen from -- that region ran unserialised and may"
                " have poisoned the saved-priority slot. Unmatched unlocks"
                " never steal; they are the SKIPPED count above\n",
                s.steals, adx_guard_timeout_ms(), s.stolen_from);
    prio_report();
    if (s.locks && s.unlocks_unmatched > s.locks)
        fprintf(stderr,
                "  [ADX-GUARD] unmatched unlocks dominate -- this is the"
                " sub_001437B0 spin calling the registered unlock\n");
    fflush(stderr);
}

void adx_guard_reset_for_test(void)
{
    pthread_mutex_lock(&g_m);
    memset(&g_st, 0, sizeof g_st);
    g_owner = 0;
    g_depth = 0;
    g_tsum = 0;
    g_parked = 0;
    pthread_mutex_unlock(&g_m);
    t_depth = 0;
    adx_prio_reset_for_test();
}

/* ── the per-thread ledger (G66) ─────────────────────────────────────────
 *
 * See adx_guard.h, WHOSE PRIORITY THE ONE SLOT HOLDS. Its own mutex and its
 * own per-thread nesting (`held`), independent of the guard: the guard's
 * t_depth is parked to zero across a blocking wait, and the ledger has to
 * know who holds a lock whether or not RECOMP_ADX_SERIALIZE is on. A thread
 * the table cannot hold (more than ADX_MAX_THREADS) gets the guest's own
 * behaviour: restore-here, no owed restore, no rescue. */

#define ADX_MAX_THREADS 64

struct adx_trec {
    unsigned long long tid;
    int held;            /* locks this thread made and has not unlocked */
    int has_saved;       /* it has raised at least once */
    int saved;           /* its pre-raise priority at its last raise */
    int owed;            /* the count reached 0 on another thread: restore */
    int owed_prio;
};

static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static struct adx_trec g_tr[ADX_MAX_THREADS];
static int      g_tr_n;
static unsigned g_tr_gen = 1;        /* bumped by the test reset */
static int      g_raiser = -1;       /* index of the thread that took 0 -> 1 */
static long     g_held_sum;          /* sum of every thread's `held` */
static struct {
    unsigned long raises, cross, owed_applied, clamps, rescues, rescue_fallback;
} g_ps;

static __thread int      t_rec = -1;
static __thread unsigned t_rec_gen;
static __thread int      t_unlock_matched = 1;  /* the last adx_prio_note_unlock */
static __thread int      t_cross_now;           /* the last restore_here went cross */

int adx_per_thread_restore_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on_default("RECOMP_ADX_PER_THREAD_RESTORE", 1);
    return on;
}

int adx_unmatched_safe_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on_default("RECOMP_ADX_UNMATCHED_SAFE", 1);
    return on;
}

int adx_trace_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_ADX_TRACE");
    return on;
}

/* This thread's record, or -1 when the table is full. Called with g_pm held. */
static int rec_index(void)
{
    if (t_rec_gen != g_tr_gen) {          /* first call, or after a reset */
        t_rec_gen = g_tr_gen;
        t_rec = g_tr_n < ADX_MAX_THREADS ? g_tr_n++ : -1;
        if (t_rec >= 0) {
            memset(&g_tr[t_rec], 0, sizeof g_tr[t_rec]);
            g_tr[t_rec].tid = adx_self_tid();
        }
    }
    return t_rec;
}

void adx_prio_note_lock(int raised, int pre_raise)
{
    int i;
    pthread_mutex_lock(&g_pm);
    i = rec_index();
    ++g_held_sum;
    if (i >= 0) {
        ++g_tr[i].held;
        if (raised) {
            g_tr[i].saved = pre_raise;
            g_tr[i].has_saved = 1;
            ++g_ps.raises;
        }
    }
    if (raised) g_raiser = i;     /* -1 when untracked: guest behaviour */
    pthread_mutex_unlock(&g_pm);
}

int adx_prio_note_unlock(void)
{
    int i, matched;
    pthread_mutex_lock(&g_pm);
    i = rec_index();
    if (i < 0) {
        matched = 1;              /* untracked: never treat as unmatched */
        --g_held_sum;
    } else if (g_tr[i].held > 0) {
        --g_tr[i].held;
        --g_held_sum;
        matched = 1;
    } else {
        matched = 0;
    }
    pthread_mutex_unlock(&g_pm);
    t_unlock_matched = matched;
    return matched;
}

int adx_prio_restore_here(void)
{
    int i, r, here = 1;
    pthread_mutex_lock(&g_pm);
    i = rec_index();
    r = g_raiser;
    g_raiser = -1;
    if (adx_per_thread_restore_on() && r >= 0 && i >= 0 && r != i) {
        /* The unlocking thread did not raise. Leave it alone and owe the
         * raiser what IT had -- today that equals the slot, but the slot is
         * shared and the ledger is not. */
        g_tr[r].owed = 1;
        g_tr[r].owed_prio = g_tr[r].saved;
        ++g_ps.cross;
        here = 0;
    }
    pthread_mutex_unlock(&g_pm);
    t_cross_now = !here;
    return here;
}

int adx_prio_take_owed(int *prio)
{
    int i, got = 0;
    if (!adx_per_thread_restore_on()) return 0;
    pthread_mutex_lock(&g_pm);
    i = rec_index();
    if (i >= 0 && g_tr[i].owed) {
        g_tr[i].owed = 0;
        if (prio) *prio = g_tr[i].owed_prio;
        ++g_ps.owed_applied;
        got = 1;
    }
    pthread_mutex_unlock(&g_pm);
    return got;
}

int adx_prio_rescue(int *prio)
{
    int i, got = 0;
    pthread_mutex_lock(&g_pm);
    i = rec_index();
    ++g_ps.rescues;
    if (i >= 0 && g_tr[i].has_saved) {
        if (prio) *prio = g_tr[i].saved;
        got = 1;
    } else {
        if (prio) *prio = 0;      /* THREAD_PRIORITY_NORMAL */
        ++g_ps.rescue_fallback;
    }
    pthread_mutex_unlock(&g_pm);
    return got;
}

void adx_prio_note_clamp(void)
{
    pthread_mutex_lock(&g_pm);
    ++g_ps.clamps;
    pthread_mutex_unlock(&g_pm);
}

/* ── the trace ring ──────────────────────────────────────────────────────── */

#define ADX_TRACE_N 256

struct adx_ev {
    unsigned long seq;
    double   t_ms;
    unsigned long long tid;
    unsigned ra;
    int      type, count_before, count_after, res, prio;
    unsigned self_depth, holder_depth;
    long     locks_minus_matched, leak, held_sum;
};

static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static struct adx_ev   g_ring[ADX_TRACE_N];
static unsigned long   g_seq;
static unsigned        g_dumped;     /* one bit per reason */
static int           (*g_count_src)(void);

void adx_trace_set_count_source(int (*read_count)(void)) { g_count_src = read_count; }

static double now_ms(void)
{
    static struct timeval t0;
    struct timeval t;
    gettimeofday(&t, NULL);
    if (!t0.tv_sec) t0 = t;
    return (double)(t.tv_sec - t0.tv_sec) * 1000.0
         + (double)(t.tv_usec - t0.tv_usec) / 1000.0;
}

static const char *ev_name(int type)
{
    switch (type) {
    case ADX_EV_LOCK:        return "lock";
    case ADX_EV_UNLOCK:      return "unlock";
    case ADX_EV_BLOCK_BEGIN: return "block_begin";
    case ADX_EV_BLOCK_END:   return "block_end";
    case ADX_EV_OWED:        return "owed_restore";
    case ADX_EV_CLAMP:       return "clamp";
    }
    return "?";
}

/* Called with g_tm held. */
static void dump_locked(const char *reason)
{
    unsigned long first = g_seq > ADX_TRACE_N ? g_seq - ADX_TRACE_N : 0, s;
    fprintf(stderr, "  [ADX-TRACE] dump (%s): last %lu of %lu events, oldest"
                    " first. count=refcount 0x25EFA0 before->after, depth=guard"
                    " nesting self/holder, res=unlock admission (1 matched,"
                    " 0 unmatched-free, -1 refused), prio=value passed to"
                    " SetThreadPriority, lmm=locks-matched, leak=lmm-live"
                    " nesting, held=ledger sum\n",
            reason, g_seq - first, g_seq);
    for (s = first; s < g_seq; ++s) {
        const struct adx_ev *e = &g_ring[s % ADX_TRACE_N];
        char cb[16], ca[16], pr[16];
        if (e->count_before == ADX_COUNT_NA) strcpy(cb, "?");
        else snprintf(cb, sizeof cb, "%d", e->count_before);
        if (e->count_after == ADX_COUNT_NA) strcpy(ca, "?");
        else snprintf(ca, sizeof ca, "%d", e->count_after);
        if (e->prio == ADX_PRIO_NONE) strcpy(pr, "-");
        else snprintf(pr, sizeof pr, "%d", e->prio);
        fprintf(stderr, "  [ADX-TRACE] %s #%lu t=%.3fms tid=%llu %-12s"
                        " ra=0x%08X count=%s->%s depth=%u/%u res=%d prio=%s"
                        " lmm=%ld leak=%ld held=%ld\n",
                reason, e->seq, e->t_ms, e->tid, ev_name(e->type), e->ra,
                cb, ca, e->self_depth, e->holder_depth, e->res, pr,
                e->locks_minus_matched, e->leak, e->held_sum);
    }
    fflush(stderr);
}

unsigned adx_trace_fired(void)
{
    unsigned m;
    pthread_mutex_lock(&g_tm);
    m = g_dumped;
    pthread_mutex_unlock(&g_tm);
    return m;
}

void adx_trace_dump(const char *reason)
{
    pthread_mutex_lock(&g_tm);
    dump_locked(reason);
    pthread_mutex_unlock(&g_tm);
}

enum { R_LEAK = 1, R_UNMATCHED = 2, R_SKIPPED = 4, R_CROSS = 8, R_CLAMP = 16,
       R_DRIFT = 32 };

void adx_trace_note(int type, unsigned ra, int count_before, int count_after,
                    int unlock_result, int prio_set)
{
    struct adx_ev e;
    unsigned fire = 0;

    if (!adx_trace_on()) { t_cross_now = 0; return; }
    memset(&e, 0, sizeof e);
    e.t_ms = now_ms();
    e.tid = adx_self_tid();
    e.ra = ra;
    e.type = type;
    e.res = unlock_result;
    e.prio = prio_set;
    if ((type == ADX_EV_BLOCK_BEGIN || type == ADX_EV_BLOCK_END) && g_count_src)
        count_before = count_after = g_count_src();
    e.count_before = count_before;
    e.count_after = count_after;

    pthread_mutex_lock(&g_m);
    e.self_depth = t_depth;
    e.holder_depth = g_depth;
    e.locks_minus_matched = (long)g_st.locks - (long)g_st.unlocks_matched;
    e.leak = e.locks_minus_matched - (g_tsum + g_parked);
    /* An admitted unlock is noted before its leave(): the level it took
     * (matched: the lock's; unmatched: its own) is still in g_tsum, and a
     * matched one is already counted. Count it as gone, or every unlock
     * reads as a leak of -1. */
    if ((type == ADX_EV_UNLOCK || type == ADX_EV_CLAMP) && unlock_result >= 0
        && adx_guard_on())
        e.leak += 1;
    pthread_mutex_unlock(&g_m);

    pthread_mutex_lock(&g_pm);
    e.held_sum = g_held_sum;
    pthread_mutex_unlock(&g_pm);

    pthread_mutex_lock(&g_tm);
    e.seq = g_seq;
    g_ring[g_seq % ADX_TRACE_N] = e;
    ++g_seq;

    /* The leak is measured by the guard, so only while the guard runs. */
    if (adx_guard_on() && e.leak != 0) fire |= R_LEAK;
    if (type == ADX_EV_UNLOCK) {
        if (unlock_result < 0) fire |= R_SKIPPED;
        else if (!t_unlock_matched) fire |= R_UNMATCHED;
        if (t_cross_now) fire |= R_CROSS;
    }
    if (type == ADX_EV_CLAMP) fire |= R_CLAMP;
    if ((type == ADX_EV_LOCK || type == ADX_EV_UNLOCK)
        && count_after != ADX_COUNT_NA && (long)count_after != e.held_sum)
        fire |= R_DRIFT;
    t_cross_now = 0;

    fire &= ~g_dumped;
    g_dumped |= fire;
    if (fire & R_LEAK)      dump_locked("leak");
    if (fire & R_UNMATCHED) dump_locked("unmatched");
    if (fire & R_SKIPPED)   dump_locked("skipped");
    if (fire & R_CROSS)     dump_locked("cross");
    if (fire & R_CLAMP)     dump_locked("clamp");
    if (fire & R_DRIFT)     dump_locked("drift");
    pthread_mutex_unlock(&g_tm);
}

/* The ledger's half of adx_guard_report. */
static void prio_report(void)
{
    unsigned long raises, cross, owed, clamps, rescues, fallback;
    pthread_mutex_lock(&g_pm);
    raises = g_ps.raises; cross = g_ps.cross; owed = g_ps.owed_applied;
    clamps = g_ps.clamps; rescues = g_ps.rescues;
    fallback = g_ps.rescue_fallback;
    pthread_mutex_unlock(&g_pm);
    fprintf(stderr,
            "  [ADX-GUARD] per-thread restore %s: raises=%lu count-to-zero on"
            " a non-raiser=%lu owed restores applied=%lu | unmatched-safe %s:"
            " unlocks at count<=0 not applied=%lu priority-15 rescues=%lu"
            " (no own saved priority, NORMAL used: %lu) | trace %s\n",
            adx_per_thread_restore_on() ? "on" : "OFF", raises, cross, owed,
            adx_unmatched_safe_on() ? "on" : "OFF", clamps, rescues, fallback,
            adx_trace_on() ? "on" : "off");
}

void adx_prio_reset_for_test(void)
{
    pthread_mutex_lock(&g_pm);
    memset(g_tr, 0, sizeof g_tr);
    g_tr_n = 0;
    ++g_tr_gen;
    g_raiser = -1;
    g_held_sum = 0;
    memset(&g_ps, 0, sizeof g_ps);
    pthread_mutex_unlock(&g_pm);
    pthread_mutex_lock(&g_tm);
    g_seq = 0;
    g_dumped = 0;
    pthread_mutex_unlock(&g_tm);
    t_unlock_matched = 1;
    t_cross_now = 0;
}
