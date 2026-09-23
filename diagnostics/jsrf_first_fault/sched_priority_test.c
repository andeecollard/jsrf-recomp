/* DOES ENFORCING PRIORITY REMOVE THE NEED FOR adx_guard?
 *
 * The ADX poisoning interleave is not an ADX bug. It is what happens when a
 * title built for ONE CPU with strict priority preemption is run with its
 * threads mapped 1:1 onto host threads that the host schedules as it likes.
 * `[THREAD] ... priority applied=0` in every session log this week: the title
 * made 557,880 KeSetBasePriority calls in one 34-minute run and we enacted
 * none of them.
 *
 * adx_guard.c restores exclusion for one routine pair with a host mutex, and
 * says so in its own header -- "NOT a general fix for priority being
 * unenacted; anything else in the title relying on TIME_CRITICAL for
 * exclusion is still exposed." handoff_guard and fedec_hold are two more
 * simulations of the same missing guarantee.
 *
 * This file asks whether the guarantee itself would do the job, on a bench,
 * with no player session and no guard:
 *
 *     scheduler OFF -> the poison MUST form.  Without that the test proves
 *                      nothing, because a model that cannot poison is green
 *                      whatever the scheduler does. This is the arm that
 *                      reproduces today's runtime.
 *     scheduler ON  -> the poison MUST NOT form, WITH adx_guard ABSENT.
 *
 * WHAT IS MODELLED, and it is deliberately the minimum. One CPU: exactly one
 * guest thread runs at a time. Strict priority: at a preemption point the
 * highest-priority runnable thread gets the CPU, and a thread that raised
 * itself keeps it. SetThreadPriority is what moves a thread's band -- that is
 * the whole point, and the thing the runtime currently drops on the floor.
 *
 * WHAT IS NOT MODELLED: time slicing between equal priorities, IRQL, DPCs,
 * and the cost of a context switch. None of them bear on whether B can enter
 * a region while A is inside it at a higher band, which is the one question
 * here. A green result is evidence for the scheduler thesis; it is NOT a
 * claim that the rewrite is free -- see (d) in the report this ships with. */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++fail; } } while (0)

/* ── the one-CPU, strict-priority scheduler ─────────────────────────────── */

#define NTHREAD 2

static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_c = PTHREAD_COND_INITIALIZER;

static int s_on;                  /* 0 = today's runtime, 1 = the thesis */
static int s_prio[NTHREAD];       /* the guest's band, as SetThreadPriority sets it */
static int s_runnable[NTHREAD];   /* has work and wants the CPU */
static int s_cpu = -1;            /* who holds the single CPU; -1 = nobody */
static unsigned long s_switches;

/* Highest band wins; ties go to the lowest index, which is enough because
 * nothing here depends on fairness between equals. Called with s_m held. */
static int pick(void)
{
    int best = -1, i;
    for (i = 0; i < NTHREAD; ++i) {
        if (!s_runnable[i]) continue;
        if (best < 0 || s_prio[i] > s_prio[best]) best = i;
    }
    return best;
}

static void sched_attach(int t, int prio)
{
    pthread_mutex_lock(&s_m);
    s_prio[t] = prio; s_runnable[t] = 1;
    if (s_on && s_cpu < 0) { s_cpu = pick(); pthread_cond_broadcast(&s_c); }
    while (s_on && s_cpu != t) pthread_cond_wait(&s_c, &s_m);
    pthread_mutex_unlock(&s_m);
}

static void sched_detach(int t)
{
    pthread_mutex_lock(&s_m);
    s_runnable[t] = 0;
    if (s_on && s_cpu == t) { s_cpu = pick(); pthread_cond_broadcast(&s_c); }
    pthread_mutex_unlock(&s_m);
}

/* A PREEMPTION POINT: where the Xbox scheduler could take the CPU away. On
 * the real machine that is any kernel entry; here it is placed exactly where
 * the guest's lock and unlock bodies could be interrupted.
 *
 * With the scheduler OFF this is a no-op, which is precisely today's
 * behaviour -- every thread runs whenever the host feels like it. */
static void sched_point(int t)
{
    if (!s_on) return;
    pthread_mutex_lock(&s_m);
    {
        int next = pick();
        if (next != s_cpu) { s_cpu = next; ++s_switches; pthread_cond_broadcast(&s_c); }
        while (s_cpu != t) pthread_cond_wait(&s_c, &s_m);
    }
    pthread_mutex_unlock(&s_m);
}

/* ── the guest's two shared words, and XAPI's half of the loop ───────────── */
/* Transcribed from adx_guard_test.c so the two files model the same defect. */

static int g_count;      /* 0x0025EFA0 */
static int g_saved;      /* 0x0027D0F8 -- ONE slot for one saved priority */
static int g_base[NTHREAD];

static int  xapi_get(int t)         { return g_base[t] == 16 ? 15 : g_base[t]; }

/* THE LINE THE RUNTIME DOES NOT HAVE. SetThreadPriority moves the guest's
 * band AND the scheduler's -- on hardware those are the same thing. Today the
 * second half is dropped, which is `priority applied=0`. */
static void xapi_set(int t, int v)
{
    g_base[t] = (v == 15) ? 16 : v;
    if (s_on) {
        pthread_mutex_lock(&s_m);
        s_prio[t] = g_base[t];
        if (s_cpu != t) { /* a raise can only take the CPU, never give it up */
            int next = pick();
            if (next != s_cpu) { s_cpu = next; pthread_cond_broadcast(&s_c); }
        }
        pthread_mutex_unlock(&s_m);
    }
}

/* The guest bodies, WITH NO GUARD OF ANY KIND. sub_0013B0A0 / sub_0013B0E0
 * as the disassembly has them, plus the preemption points hardware would
 * have. If the scheduler is the fix, these need nothing else. */
static void guest_lock(int t)
{
    sched_point(t);
    if (g_count == 0) {          /* the `jne` */
        g_saved = xapi_get(t);
        xapi_set(t, 15);
    }
    sched_point(t);
    ++g_count;
}

static void guest_unlock(int t)
{
    sched_point(t);
    --g_count;
    if (g_count == 0)            /* the `jne` */
        xapi_set(t, g_saved);
    sched_point(t);
}

/* ── driving it ─────────────────────────────────────────────────────────────
 *
 * NOT a step gate. The first two versions of this file drove the interleave
 * from a third thread, and both were wrong for the same reason: a step gate
 * makes A WAIT inside its own critical region, and a thread that waits gives
 * up the CPU. On hardware A raises to 16 and runs the region straight
 * through -- it never parks in the middle. Gating it there models the one
 * thing that cannot happen and hides the property under test.
 *
 * So: A runs its region to completion while B spins trying to enter. The
 * question is simply whether B's body can execute while A is raised, and B
 * records that itself. */

static volatile int a_in_region;     /* A is between its lock and its unlock */
static volatile int a_done;
static unsigned long b_entries;      /* B ran its lock body */
static unsigned long b_entries_while_a_raised;   /* ...and A was at 16. FATAL. */

static void work(int t, int rounds)
{
    int i;
    for (i = 0; i < rounds; ++i) {
        volatile int spin; int k;
        for (k = 0; k < 120; ++k) spin = k;
        (void)spin;
        sched_point(t);              /* preemptible throughout, as on hardware */
    }
}

/* A takes the region repeatedly, as the title does 200,000 times a session.
 * The poison needs B's unlock to land AFTER A's, and then A to lock again --
 * an ordering that arises on its own once both threads cycle. Forcing it with
 * waits would be wrong: under the scheduler A would be waiting for a thread
 * it is deliberately excluding, which is a deadlock of the test's making. */
static void *thread_a(void *u)
{
    int r;
    (void)u;
    sched_attach(0, 8);
    for (r = 0; r < 60 && g_saved != 15; ++r) {
        guest_lock(0);
        a_in_region = 1;
        work(0, 400);
        a_in_region = 0;
        guest_unlock(0);
        work(0, 60);
    }
    a_done = 1;
    sched_detach(0);
    return NULL;
}

static void *thread_b(void *u)
{
    int i;
    (void)u;
    sched_attach(1, 8);
    for (i = 0; i < 4000000 && !a_done; ++i) {
        sched_point(1);
        if (a_in_region) {
            int raised = (g_base[0] == 16);
            ++b_entries;
            if (raised) ++b_entries_while_a_raised;
            guest_lock(1);           /* step 2: B enters A's region */
            work(1, 900);            /* B holds it LONGER than A's remainder,
                                      * so A's unlock (step 3) lands first */
            guest_unlock(1);         /* step 4 */
        }
    }
    sched_detach(1);
    return NULL;
}

static void run_arm(int scheduler)
{
    pthread_t a, b;

    s_on = scheduler; s_cpu = -1; s_switches = 0;
    memset(s_runnable, 0, sizeof s_runnable);
    g_count = 0; g_saved = 0; g_base[0] = 8; g_base[1] = 8;
    a_in_region = a_done = 0;
    b_entries = b_entries_while_a_raised = 0;

    pthread_create(&a, NULL, thread_a, NULL);
    pthread_create(&b, NULL, thread_b, NULL);
    pthread_join(a, NULL);
    a_done = 1;
    pthread_join(b, NULL);
}

int main(int argc, char **argv)
{
    int only = argc > 1 ? (strcmp(argv[1], "on") == 0 ? 1 : 0) : -1;

    if (only != 1) {
        /* POSITIVE CONTROL: today's runtime, priority recorded and not
         * enacted. B must get into the region and the poison must form, or
         * this file is measuring nothing. */
        run_arm(0);
        CHECK(b_entries_while_a_raised > 0,
              "control arm: B never entered while A was raised (%lu entries),"
              " so the model does not reproduce today's runtime", b_entries);
        CHECK(g_saved == 15,
              "control arm: the poison did NOT form (saved=%d)", g_saved);
        fprintf(stderr, "scheduler off: saved=%d  B entered while A raised:"
                        " %lu of %lu  <- the defect, reproduced\n",
                g_saved, b_entries_while_a_raised, b_entries);
    }

    if (only != 0) {
        run_arm(1);
        CHECK(b_entries_while_a_raised == 0,
              "B ENTERED THE REGION %lu TIME(S) WHILE A WAS AT BAND 16 --"
              " enforcing priority did not exclude it",
              b_entries_while_a_raised);
        CHECK(g_saved != 15,
              "THE POISON FORMED WITH THE SCHEDULER ON (saved=%d): enforcing"
              " priority is NOT sufficient and adx_guard is still needed",
              g_saved);
        CHECK(g_base[0] == 8, "A was not restored to its own band, base=%d",
              g_base[0]);
        fprintf(stderr, "scheduler on:  saved=%d base_a=%d  B entered while A"
                        " raised: %lu  switches=%lu  <- NO GUARD PRESENT\n",
                g_saved, g_base[0], b_entries_while_a_raised, s_switches);
    }

    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail ? 1 : 0;
}
