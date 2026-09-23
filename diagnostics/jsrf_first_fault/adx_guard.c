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
            ++t_depth;
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
            ++t_depth;          /* not "= 1": a steal victim may re-enter */
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
            ++t_depth;
            g_depth = t_depth;
            got = 1;
        } else if (g_owner == 0) {
            g_owner = me;
            g_owner_tid = adx_self_tid();
            ++t_depth;
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
        --t_depth;
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
    t_depth = saved_depth;                 /* the nesting the wait interrupted */
    g_depth = saved_depth;
    if (t_depth > g_st.max_depth) g_st.max_depth = t_depth;
    g_st.depth = g_depth;
    pthread_mutex_unlock(&g_m);
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
    pthread_mutex_unlock(&g_m);
    t_depth = 0;
}
