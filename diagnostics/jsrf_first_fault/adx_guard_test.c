/* THE POISONING INTERLEAVE, FORCED.
 *
 * The handover that named the ADX freeze wrote the sequence down:
 *
 *     A locks    count 0->1, A raised to 16, A's real priority saved
 *     B locks    count 1->2, the `jne` skips, B untouched
 *     A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
 *     B unlocks  count 1->0, restores A's saved value onto B
 *     A locks    A is already at 16, so it saves 15.  POISONED.
 *
 * and then claimed a fix that could not prevent it, because every step there
 * is a whole call and the steps are already serial. This test exists so that
 * claim cannot be made again by reading: it drives the five steps from a third
 * thread, with the guest's two shared words and XAPI's priority mapping
 * modelled exactly, and asks what `saved` holds at the end.
 *
 *     off / 0   the sequence runs to completion and saved == 15 -- the poison
 *               forms, which is the positive control. Without it a guarded run
 *               proves nothing, because a test that never poisons is green
 *               whatever the guard does.
 *     on        step 2 BLOCKS. The proof is not that the numbers came out
 *               right, it is that B could not enter the region at all.
 *
 * The `off` and `0` arms are also the switch-grammar check: presence testing
 * read `=0` as ON, so an A/B whose control arm set the variable to 0 was two
 * identical arms. Here the two arms must agree, and they can only agree if
 * recomp_switch_on() is doing the reading.
 */
#include "adx_guard.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* ── the guest's state, and XAPI's half of the loop ──────────────────────── */

static int g_count;          /* 0x0025EFA0, the refcount */
static int g_saved;          /* 0x0027D0F8, ONE slot for one saved priority */
static int g_base[2];        /* each thread's base priority */

/* XAPI maps base 16 -> 15 on the way out and 15 -> base 16 on the way in. The
 * round trip being EXACT is what sustains the spin in the real defect, so the
 * model keeps it exact. */
static int xapi_get(int t)          { return g_base[t] == 16 ? 15 : g_base[t]; }
static void xapi_set(int t, int v)  { g_base[t] = (v == 15) ? 16 : v; }

static void guest_lock(int t)
{
    adx_guard_lock_enter();
    if (g_count == 0) {                 /* the `jne` */
        g_saved = xapi_get(t);
        xapi_set(t, 15);
    }
    ++g_count;
}

static void guest_unlock(int t)
{
    /* Mirrors sub_0013B0E0: a refused pass returns without touching either
     * shared word and without a leave, because nothing was taken. */
    if (adx_guard_unlock_enter() < 0) return;
    --g_count;
    if (g_count == 0)                   /* the `jne` */
        xapi_set(t, g_saved);
    adx_guard_unlock_leave();
}

/* ── a step gate, so the interleave is driven and not raced ──────────────── */

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
static int  step;            /* the step the driver has released */
static int  done;            /* BITMASK of finished steps, not a high-water
                              * mark: the guarded arm finishes 3 before 2, and
                              * a max would report 2 as reached the moment 3
                              * was -- which is exactly the observation this
                              * test turns on. */

static void release_step(int n)
{
    pthread_mutex_lock(&m);
    step = n;
    pthread_cond_broadcast(&c);
    pthread_mutex_unlock(&m);
}

static void await_step(int n)
{
    pthread_mutex_lock(&m);
    while (step < n) pthread_cond_wait(&c, &m);
    pthread_mutex_unlock(&m);
}

static void mark_done(int n)
{
    pthread_mutex_lock(&m);
    done |= 1 << n;
    pthread_cond_broadcast(&c);
    pthread_mutex_unlock(&m);
}

/* Did step `n` finish within `ms`? This is the only timed assertion here, and
 * it is one-sided on purpose: a guard that works makes it time out, and a
 * guard that does not cannot make it time out by being slow, because step 2 is
 * an uncontended lock of a free mutex. */
static int done_within(int n, int ms)
{
    struct timeval now;
    struct timespec dl;
    int ok;

    gettimeofday(&now, NULL);
    dl.tv_sec  = now.tv_sec + ms / 1000;
    dl.tv_nsec = now.tv_usec * 1000L + (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec += 1; dl.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&m);
    while (!(done & (1 << n)) && pthread_cond_timedwait(&c, &m, &dl) == 0)
        ;
    ok = (done & (1 << n)) != 0;
    pthread_mutex_unlock(&m);
    return ok;
}

static void *thread_a(void *unused)
{
    (void)unused;
    await_step(1); guest_lock(0);   mark_done(1);   /* A locks   */
    await_step(3); guest_unlock(0); mark_done(3);   /* A unlocks */
    await_step(5); guest_lock(0);   mark_done(5);   /* A locks again */
    return NULL;
}

static void *thread_b(void *unused)
{
    (void)unused;
    await_step(2); guest_lock(1);   mark_done(2);   /* B locks   */
    await_step(4); guest_unlock(1); mark_done(4);   /* B unlocks */
    return NULL;
}

static int fail;
#define CHECK(cond, ...) do {                                           \
        if (!(cond)) { fail = 1;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }        \
    } while (0)

/* ── the interleave ─────────────────────────────────────────────────────── */

static int run_interleave(int guarded)
{
    pthread_t a, b;
    int b_entered;

    g_count = 0; g_saved = 0;
    g_base[0] = 8; g_base[1] = 9;      /* distinct, and neither is 16 */
    step = 0; done = 0;
    adx_guard_reset_for_test();

    pthread_create(&a, NULL, thread_a, NULL);
    pthread_create(&b, NULL, thread_b, NULL);

    release_step(1);
    CHECK(done_within(1, 5000), "A's lock never completed");
    CHECK(g_base[0] == 16, "A should be raised to base 16, is %d", g_base[0]);
    CHECK(g_saved == 8, "A's real priority should be saved, saved=%d", g_saved);

    /* THE STEP THAT DECIDES IT. */
    release_step(2);
    b_entered = done_within(2, 400);

    if (guarded) {
        CHECK(!b_entered,
              "B ENTERED THE REGION WHILE A HELD IT -- the guard does not span"
              " the guest critical section");
        /* Let A out; B's lock is queued behind it and completes now. */
        release_step(3);
        CHECK(done_within(3, 5000), "A's unlock never completed");
        CHECK(done_within(2, 5000), "B's lock never unblocked after A's unlock");
        release_step(4);
        CHECK(done_within(4, 5000), "B's unlock never completed");
        release_step(5);
        CHECK(done_within(5, 5000), "A's second lock never completed");

        CHECK(g_saved != 15,
              "saved==15: the poison formed even with the guard on");
        CHECK(g_saved == 8,
              "A's second lock should save its own 8, saved=%d", g_saved);
        CHECK(g_base[1] == 9,
              "B should still hold its own base 9, is %d", g_base[1]);
    } else {
        CHECK(b_entered,
              "B did not enter while A held the region -- the control arm did"
              " not reproduce, so a guarded pass would prove nothing."
              " Is RECOMP_ADX_SERIALIZE set?");
        release_step(3); CHECK(done_within(3, 5000), "A's unlock stalled");
        release_step(4); CHECK(done_within(4, 5000), "B's unlock stalled");
        release_step(5); CHECK(done_within(5, 5000), "A's relock stalled");

        CHECK(g_saved == 15,
              "the poison did NOT form unguarded (saved=%d) -- the model no"
              " longer reproduces the defect it is guarding", g_saved);
    }

    pthread_join(a, NULL);
    pthread_join(b, NULL);
    return fail;
}

/* ── recursion, and the unlocks the guest never matched ─────────────────── */

static void run_recursive(void)
{
    struct adx_guard_stats s;

    g_count = 0; g_saved = 0; g_base[0] = 8;
    adx_guard_reset_for_test();

    /* A guest routine reached from inside the pair calls back into it. On an
     * Xbox that is free -- the caller is already at 16. It must be free here
     * too, or the fix converts a livelock into a deadlock. */
    guest_lock(0);
    guest_lock(0);
    CHECK(g_count == 2, "nested lock should count 2, counts %d", g_count);
    guest_unlock(0);
    CHECK(g_count == 1, "inner unlock should leave 1, leaves %d", g_count);
    guest_unlock(0);
    CHECK(g_count == 0, "outer unlock should leave 0, leaves %d", g_count);
    CHECK(g_base[0] == 8, "priority should be restored, base=%d", g_base[0]);

    adx_guard_read_stats(&s);
    if (adx_guard_on()) {
        CHECK(s.max_depth == 2, "guard depth should reach 2, reached %u",
              s.max_depth);
        CHECK(s.owner == 0, "guard should be free after the outer unlock");
        CHECK(s.steals == 0, "no steal should be possible here, saw %lu",
              s.steals);
    }
}

static void *unmatched_unlocker(void *unused)
{
    (void)unused;
    CHECK(adx_guard_unlock_enter() == 0,
          "an unlock with no matching lock reported itself as matched");
    adx_guard_unlock_leave();
    return NULL;
}

static void run_unmatched(void)
{
    struct adx_guard_stats s;
    pthread_t t;

    adx_guard_reset_for_test();

    /* sub_001437B0's spin calls the registered unlock on every pass, from a
     * thread that never locked. Releasing on those would hand a region that
     * belongs to somebody else away; the guard must take it for the body and
     * give it straight back. Nobody holds it here, so this cannot block. */
    pthread_create(&t, NULL, unmatched_unlocker, NULL);
    pthread_join(t, NULL);

    adx_guard_read_stats(&s);
    if (adx_guard_on()) {
        CHECK(s.unlocks_unmatched == 1,
              "the unmatched unlock was not counted (%lu)", s.unlocks_unmatched);
        CHECK(s.unlocks_matched == 0,
              "it was counted as matched (%lu)", s.unlocks_matched);
        CHECK(s.owner == 0 && s.depth == 0,
              "the guard was left held by an unmatched unlock");
    }
}

/* ── session 10: an unmatched unlock while A is inside the region ───────── */

static int r_contended;      /* what adx_guard_unlock_enter() told thread B */

static void *contended_unmatched(void *unused)
{
    (void)unused;
    /* B never locked, and A is inside its region. Under the guard this must
     * be refused outright. The old code waited adx_guard_timeout_ms() and
     * then STOLE, which let this body run underneath A -- the whole defect. */
    r_contended = adx_guard_unlock_enter();
    if (r_contended >= 0) {             /* let in: run the body it would run */
        --g_count;
        if (g_count == 0) xapi_set(1, g_saved);
        adx_guard_unlock_leave();
    }
    mark_done(6);
    return NULL;
}

static void run_unmatched_contended(int guarded)
{
    struct adx_guard_stats s;
    pthread_t t;

    g_count = 0; g_saved = 0; g_base[0] = 1; g_base[1] = 1;
    step = 0; done = 0; r_contended = 99;
    adx_guard_reset_for_test();

    guest_lock(0);          /* A: count 0->1, saves its real priority 1 */
    CHECK(g_saved == 1, "A's lock should have saved 1, saved %d", g_saved);
    CHECK(g_base[0] == 16, "A's lock should have raised it, base=%d", g_base[0]);

    pthread_create(&t, NULL, contended_unmatched, NULL);

    /* PROMPTLY, in both arms. A refusal that takes the timeout is still the
     * 5000 ms frame the player watched, so "not stolen" is not enough on its
     * own -- it also has to not wait. */
    CHECK(done_within(6, 1000),
          "the unmatched unlock neither ran nor was refused within 1 s --"
          " it is waiting out the %u ms timeout", adx_guard_timeout_ms());
    pthread_join(t, NULL);

    if (guarded)
        CHECK(r_contended < 0,
              "a contended unmatched unlock was let into the region (%d)",
              r_contended);

    /* A's nested lock, the step that poisons. Guarded, the count is still 1,
     * so the `jne` skips and the slot is never rewritten. */
    guest_lock(0);

    adx_guard_read_stats(&s);
    if (guarded) {
        CHECK(g_saved == 1,
              "THE POISON FORMED UNDER THE GUARD: saved=%d (want 1)", g_saved);
        CHECK(g_count == 2, "the refused pass changed the count (%d)", g_count);
        CHECK(s.unlocks_skipped == 1,
              "the refused pass was not counted (%lu)", s.unlocks_skipped);
        CHECK(s.steals == 0,
              "an unmatched unlock stole the guard (%lu steals)", s.steals);
        guest_unlock(0);
        guest_unlock(0);
        CHECK(g_base[0] == 1, "A was not restored, base=%d", g_base[0]);
    } else {
        /* The positive control. Without the guard this exact sequence is the
         * one the log recorded, and it must still reach 15 -- otherwise the
         * test has stopped modelling the thing it guards. */
        CHECK(g_saved == 15,
              "the poison did NOT form unguarded (saved=%d) -- this sequence"
              " no longer reproduces session 10", g_saved);
    }
}

/* A HOLDER THAT BLOCKS STOPS EXCLUDING, AND RE-TAKES ITS NESTING ON WAKING.
 * Priority elevation only excludes while the elevated thread runs: see
 * adx_guard.h, THE GUARD IS PRIORITY. A holds the guard two deep and "blocks";
 * B must then get in, and A's wake must wait for B to leave and come back two
 * deep. The 23 Sep 17:13 session froze because this could not happen. */
static volatile int blk_stage;
static unsigned blk_saved;
static void *blk_a(void *u)
{
    (void)u;
    adx_guard_lock_enter(); adx_guard_lock_enter();      /* depth 2 */
    blk_stage = 1;
    while (blk_stage < 2) usleep(1000);
    blk_saved = adx_guard_block_begin();                 /* A blocks in a kernel wait */
    blk_stage = 3;
    while (blk_stage < 4) usleep(1000);                  /* B is inside now */
    adx_guard_block_end(blk_saved);                      /* must wait for B */
    blk_stage = 6;
    { struct adx_guard_stats st; adx_guard_read_stats(&st); blk_saved = st.depth; }
    if (adx_guard_unlock_enter()) adx_guard_unlock_leave();
    if (adx_guard_unlock_enter()) adx_guard_unlock_leave();
    return NULL;
}
static void *blk_b(void *u)
{
    (void)u;
    while (blk_stage < 1) usleep(1000);
    blk_stage = 2;
    adx_guard_lock_enter();                              /* blocks until A blocks */
    blk_stage = 4;
    usleep(150 * 1000);                                  /* A wakes meanwhile and must wait */
    blk_stage = 5;
    if (adx_guard_unlock_enter()) adx_guard_unlock_leave();
    return NULL;
}
static void run_block_release(int guarded)
{
    pthread_t a, b; int i;
    if (!guarded) return;
    adx_guard_reset_for_test();
    CHECK(adx_guard_block_begin() == 0, "a thread holding nothing must get 0 back");
    adx_guard_block_end(0);
    blk_stage = 0;
    pthread_create(&a, NULL, blk_a, NULL);
    pthread_create(&b, NULL, blk_b, NULL);
    for (i = 0; i < 5000 && blk_stage < 4; ++i) usleep(1000);
    CHECK(blk_stage >= 4, "B never got in while A was blocked (stage %d)", blk_stage);
    CHECK(blk_saved == 2, "A's nesting at the block should be 2, saved %u", blk_saved);
    for (i = 0; i < 100 && blk_stage < 5; ++i) {
        CHECK(blk_stage != 6, "A re-took the guard while B was still inside");
        usleep(1000);
    }
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(blk_stage == 6, "A never woke (stage %d)", blk_stage);
    CHECK(blk_saved == 2, "A should be two deep again after waking, is %u", blk_saved);
    { struct adx_guard_stats st; adx_guard_read_stats(&st);
      CHECK(st.depth == 0 && st.owner == 0, "guard not free at the end (depth %u owner %lu)", st.depth, st.owner);
      CHECK(st.block_releases == 1, "block_releases should be 1, is %lu", st.block_releases); }
}

int main(int argc, char **argv)
{
    int guarded = adx_guard_on();
    const char *want = argc > 1 ? argv[1] : NULL;

    /* The arm names what it expected, so a run that read the environment the
     * other way round fails here rather than reporting a green A/B. */
    if (want) {
        int expect = strcmp(want, "on") == 0;
        if (expect != guarded) {
            fprintf(stderr,
                    "FAIL: arm '%s' but RECOMP_ADX_SERIALIZE reads %s."
                    " A presence test reads =0 as on; recomp_switch_on does"
                    " not.\n", want, guarded ? "on" : "off");
            return 1;
        }
    }
    fprintf(stderr, "arm: guard %s\n", guarded ? "ON" : "off");

    run_interleave(guarded);
    run_recursive();
    run_unmatched();
    run_unmatched_contended(guarded);
    run_block_release(guarded);
    adx_guard_report();

    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail;
}
