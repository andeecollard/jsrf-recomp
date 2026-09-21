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
                        timed_out = 1;
                        break;
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
            ++t_depth;          /* not "= 1": a steal victim may re-enter */
            g_depth = t_depth;
        }
        if (t_depth > g_st.max_depth) g_st.max_depth = t_depth;
        g_st.depth = g_depth;
        g_st.owner = g_owner;
    }
    pthread_mutex_unlock(&g_m);
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
    int matched;
    if (!adx_guard_on()) return 0;

    matched = (t_depth > 0);
    if (!matched)
        guard_acquire();        /* serialise this body; own nothing after it */

    pthread_mutex_lock(&g_m);
    if (matched) ++g_st.unlocks_matched;
    else         ++g_st.unlocks_unmatched;
    pthread_mutex_unlock(&g_m);
    return matched;
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
            " (+%lu UNMATCHED) contended=%lu max_depth=%u held=%u\n",
            adx_guard_on() ? "on" : "OFF",
            s.locks, s.unlocks_matched, s.unlocks_unmatched,
            s.contended, s.max_depth, s.depth);
    /* stolen_from rides on the same line as steals rather than a line of its
     * own: it is the other half of one event, and a counter that is collected
     * and never printed is one this tree has had to retire before. */
    if (s.steals || s.stolen_from)
        fprintf(stderr,
                "  [ADX-GUARD] %lu STEALS after %u ms, %lu releases by a thread"
                " already stolen from -- a lock was held with no unlock, and"
                " that region ran unserialised\n",
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
