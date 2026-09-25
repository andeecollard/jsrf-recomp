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

/* Both mirror jsrf_manual_overrides.c call for call, including the G66
 * ledger's decisions (adx_guard.h, WHOSE PRIORITY THE ONE SLOT HOLDS): an owed
 * restore is collected at entry, the lock tells the ledger whether it raised,
 * and the unlock asks it whether the slot is this thread's to restore. */
static void guest_lock(int t)
{
    int p, raised = 0, pre = 0;
    if (adx_prio_take_owed(&p)) xapi_set(t, p);
    adx_guard_lock_enter();
    if (g_count == 0) {                 /* the `jne` */
        pre = xapi_get(t);
        g_saved = pre;
        xapi_set(t, 15);
        raised = 1;
    }
    ++g_count;
    adx_prio_note_lock(raised, pre);
    adx_trace_note(ADX_EV_LOCK, 0, g_count - 1, g_count, 0,
                   raised ? 15 : ADX_PRIO_NONE);
}

/* Returns what adx_guard_unlock_enter() said. */
static int guest_unlock(int t)
{
    int p, r, matched, prio_set = ADX_PRIO_NONE;
    if (adx_prio_take_owed(&p)) xapi_set(t, p);
    /* Mirrors sub_0013B0E0: a refused pass returns without touching either
     * shared word and without a leave, because nothing was taken. */
    r = adx_guard_unlock_enter();
    if (r < 0) return r;
    matched = adx_prio_note_unlock();
    if (g_count <= 0 && adx_unmatched_safe_on()) {
        adx_prio_note_clamp();
        if (!matched && xapi_get(t) == 15) {
            adx_prio_rescue(&p);
            xapi_set(t, p);
            prio_set = p;
        }
        adx_trace_note(ADX_EV_UNLOCK, 0, g_count, g_count, r, prio_set);
        adx_trace_note(ADX_EV_CLAMP, 0, g_count, g_count, r, prio_set);
        adx_guard_unlock_leave();
        return r;
    }
    --g_count;
    if (g_count == 0 && adx_prio_restore_here()) {  /* the `jne` */
        xapi_set(t, g_saved);
        prio_set = g_saved;
    }
    adx_trace_note(ADX_EV_UNLOCK, 0, g_count + 1, g_count, r, prio_set);
    adx_guard_unlock_leave();
    return r;
}

/* WHAT THE ASSERTIONS EXPECT, kept apart from what the model DOES. The model
 * (guest_lock/guest_unlock) always follows the real switches, as the
 * overrides do. The assertions follow the arm named on the command line when
 * there is one, so the negative-control ctest can name "fix", run with both
 * switches at 0, and must FAIL -- which is the proof that the fix assertions
 * can see the defect at all. */
static int want_fix = -1;
static int expect_ptr(void)  { return want_fix >= 0 ? want_fix : adx_per_thread_restore_on(); }
static int expect_safe(void) { return want_fix >= 0 ? want_fix : adx_unmatched_safe_on(); }

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

        /* Unguarded, B's unlock takes the count to 0 and B is not the
         * raiser. Guest behaviour restores A's 8 onto B and A saves 15 at
         * step 5 -- the positive control. The per-thread restore leaves B
         * alone and owes A its 8, which A collects at step 5 before it
         * saves: the same fix closes the original 21 Sep interleave. */
        if (expect_ptr()) {
            CHECK(g_saved == 8,
                  "per-thread restore: A should save its own 8, saved=%d",
                  g_saved);
            CHECK(g_base[1] == 9, "B should keep its own 9, has %d",
                  g_base[1]);
        } else {
            CHECK(g_saved == 15,
                  "the poison did NOT form unguarded (saved=%d) -- the model"
                  " no longer reproduces the defect it is guarding", g_saved);
        }
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
    r_contended = guest_unlock(1);
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
    } else if (expect_ptr()) {
        /* B's unmatched unlock took the count 1 -> 0 but B did not raise, so
         * B is left alone and A is owed its 1 -- collected before A's nested
         * lock saves, so the slot holds 1 and not 15. */
        CHECK(g_saved == 1,
              "per-thread restore: A should save its own 1, saved=%d", g_saved);
        CHECK(g_base[1] == 1, "B was given a priority it never had (%d)",
              g_base[1]);
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

/* ── G66: the block-release window hands A's priority to M ───────────────
 *
 * The interleave behind "A16 M1" in every poisoned KeSetBasePriority trace
 * (adx_guard.h, WHOSE PRIORITY THE ONE SLOT HOLDS), driven step by step. The
 * blocking waits are the kernel's hooks, called exactly where
 * bridge_KeWaitForSingleObject calls them. Guard on, each step is admitted by
 * the block-release rule; guard off, the steps are simply serial -- the guest
 * words see the same sequence either way, which is why this runs in every
 * arm.
 *
 *     1 A locks        count 0->1, slot := A's 8, A raised to 16
 *     2 A blocks       the guard lets go
 *     3 M locks        count 1->2, the `jne` skips
 *     4 M blocks       the guard lets go
 *     5 A wakes
 *     6 A unlocks      count 2->1, the `jne` skips -- A still at 16
 *     7 M wakes
 *     8 M unlocks      count 1->0: guest restores A's 8 ONTO M
 *     9 A locks again  guest: A still at 16, saves 15.  POISONED.
 */
static unsigned w_saved_a, w_saved_m;
static int w_base_a_after8, w_base_m_after8, w_saved_after8;

static void *win_a(void *u)
{
    (void)u;
    await_step(1); guest_lock(0);                         mark_done(1);
    await_step(2); w_saved_a = adx_guard_block_begin();   mark_done(2);
    await_step(5); adx_guard_block_end(w_saved_a);        mark_done(5);
    await_step(6); guest_unlock(0);                       mark_done(6);
    await_step(9); guest_lock(0);                         mark_done(9);
    await_step(10); guest_unlock(0);                      mark_done(10);
    return NULL;
}

static void *win_m(void *u)
{
    (void)u;
    await_step(3); guest_lock(1);                         mark_done(3);
    await_step(4); w_saved_m = adx_guard_block_begin();   mark_done(4);
    await_step(7); adx_guard_block_end(w_saved_m);        mark_done(7);
    await_step(8); guest_unlock(1);
    w_base_a_after8 = g_base[0];
    w_base_m_after8 = g_base[1];
    w_saved_after8  = g_saved;                            mark_done(8);
    return NULL;
}

static void run_block_window(int guarded)
{
    pthread_t a, mt;
    int n;

    g_count = 0; g_saved = 0;
    g_base[0] = 8; g_base[1] = 9;       /* A (the ADX thread), M (main) */
    step = 0; done = 0;
    adx_guard_reset_for_test();

    pthread_create(&a, NULL, win_a, NULL);
    pthread_create(&mt, NULL, win_m, NULL);
    for (n = 1; n <= 10; ++n) {
        release_step(n);
        CHECK(done_within(n, 3000), "block window: step %d never completed"
              " (guard %s)", n, guarded ? "on" : "off");
        if (n == 1) {
            CHECK(g_saved == 8 && g_base[0] == 16,
                  "step 1: A should save 8 and sit at 16 (saved=%d base=%d)",
                  g_saved, g_base[0]);
        }
        if (n == 3)
            CHECK(g_count == 2, "step 3: M should be in, count=%d", g_count);
    }
    pthread_join(a, NULL);
    pthread_join(mt, NULL);

    if (guarded)
        CHECK(w_saved_a == 1 && w_saved_m == 1,
              "both holders should have parked depth 1 (A %u, M %u)",
              w_saved_a, w_saved_m);

    if (expect_ptr()) {
        CHECK(w_base_m_after8 == 9,
              "FIX: M's unlock to 0 must leave M's own 9, M has %d",
              w_base_m_after8);
        CHECK(w_saved_after8 == 8, "the guest slot must still hold what A's"
              " lock wrote (8), holds %d", w_saved_after8);
        CHECK(g_saved == 8,
              "FIX: A's relock must save A's own 8 (owed restore collected"
              " first), saved=%d", g_saved);
        CHECK(g_base[0] == 8, "A should be back at 8 after its own unlock,"
              " is %d", g_base[0]);
        if (adx_trace_on())
            CHECK(adx_trace_fired() & 8, "the trace did not dump 'cross'");
    } else {
        /* THE NEGATIVE CONTROL: the defect as the logs recorded it. */
        CHECK(w_base_m_after8 == 8,
              "CONTROL: without the fix M should receive A's 8, has %d"
              " -- the model no longer reproduces A16 M1", w_base_m_after8);
        CHECK(w_base_a_after8 == 16,
              "CONTROL: A should be left at 16, is %d", w_base_a_after8);
        CHECK(g_saved == 15,
              "CONTROL: A's relock should save 15 (poisoned), saved=%d",
              g_saved);
    }
}

/* ── G66: CRI's drop-all-nesting loop must terminate ─────────────────────
 *
 * sub_001437B0: `while (GetThreadPriority(self) == 15) unlock();`. The thread
 * got 15 from a poisoned slot, holds nothing, and the count is 0. Without the
 * fix every pass decrements, the count runs negative, no pass ever restores,
 * and the loop never ends (bounded here at 1000 passes). */
static void run_spin_breaker(void)
{
    int passes;

    g_count = 0; g_saved = 0; g_base[1] = 0;     /* M: NORMAL */
    adx_guard_reset_for_test();

    guest_lock(1);                     /* M's own raise: saves 0 */
    guest_unlock(1);
    CHECK(g_base[1] == 0 && g_count == 0, "setup: M should be back at 0");

    g_base[1] = 16;                    /* 15 handed to M by a poisoned slot */
    for (passes = 0; passes < 1000 && xapi_get(1) == 15; ++passes)
        guest_unlock(1);

    if (expect_safe()) {
        CHECK(passes == 1, "FIX: the loop should exit after one pass, took"
              " %d", passes);
        CHECK(g_count == 0, "FIX: the count must not go below 0, is %d",
              g_count);
        CHECK(g_base[1] == 0, "FIX: M should get its own 0 back, has %d",
              g_base[1]);
        if (adx_trace_on())
            CHECK(adx_trace_fired() & 16, "the trace did not dump 'clamp'");
    } else {
        CHECK(passes == 1000, "CONTROL: without the fix the loop should spin"
              " (bounded at 1000), exited after %d", passes);
        CHECK(g_count == -1000, "CONTROL: the count should run to -1000, is"
              " %d", g_count);
    }

    /* A thread that never raised has no saved priority of its own: NORMAL. */
    if (expect_safe()) {
        adx_guard_reset_for_test();
        g_count = 0; g_base[1] = 16;
        for (passes = 0; passes < 1000 && xapi_get(1) == 15; ++passes)
            guest_unlock(1);
        CHECK(passes == 1 && g_base[1] == 0 && g_count == 0,
              "FIX, no own raise: should fall back to NORMAL in one pass"
              " (passes %d base %d count %d)", passes, g_base[1], g_count);
    }
}

/* ── the trace's leak test sees a nesting lost across a blocking wait ─────
 *
 * Guest code run from inside a blocking wait (a DPC from the wait loop, on the
 * same host thread) that locks and has not unlocked when the wait ends: the
 * wait's block_end overwrites t_depth with the depth parked at block_begin,
 * and the inner level is gone. One candidate for G66's lock leak; this proves
 * only that the trace would name it, not that it happens in the title. */
static void run_leak_detector(int guarded)
{
    unsigned parked;
    if (!guarded || !adx_trace_on()) return;
    adx_guard_reset_for_test();
    g_count = 0; g_base[0] = 1;

    guest_lock(0);
    CHECK(!(adx_trace_fired() & 1), "leak dumped with nothing lost");
    parked = adx_guard_block_begin();
    guest_lock(0);                     /* inside the wait */
    CHECK(!(adx_trace_fired() & 1), "leak dumped before anything was lost");
    adx_guard_block_end(parked);       /* the inner level is overwritten */
    CHECK(adx_trace_fired() & 1,
          "a nesting lost across block_end was not reported as a leak");
    guest_unlock(0);
    guest_unlock(0);
    adx_guard_reset_for_test();
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
    /* The second word names the G66 fixes' expected state the same way:
     * "fix" = both on (their default), "nofix" = both off, the negative
     * control. Without it an arm meant to be the control could quietly run
     * the fix and pass. */
    if (argc > 2) {
        int expect = strcmp(argv[2], "fix") == 0;
        want_fix = expect;
        /* "force" skips the agreement check: the negative control. */
        int force = argc > 3 && strcmp(argv[3], "force") == 0;
        if (!force && (adx_per_thread_restore_on() != expect
                       || adx_unmatched_safe_on() != expect)) {
            fprintf(stderr,
                    "FAIL: arm '%s' but RECOMP_ADX_PER_THREAD_RESTORE reads %s"
                    " and RECOMP_ADX_UNMATCHED_SAFE reads %s\n", argv[2],
                    adx_per_thread_restore_on() ? "on" : "off",
                    adx_unmatched_safe_on() ? "on" : "off");
            return 1;
        }
    }
    fprintf(stderr, "arm: guard %s, per-thread restore %s, unmatched-safe %s,"
                    " trace %s\n", guarded ? "ON" : "off",
            adx_per_thread_restore_on() ? "ON" : "off",
            adx_unmatched_safe_on() ? "ON" : "off",
            adx_trace_on() ? "ON" : "off");

    run_interleave(guarded);
    run_recursive();
    run_unmatched();
    run_unmatched_contended(guarded);
    run_block_release(guarded);
    run_block_window(guarded);
    run_spin_breaker();
    run_leak_detector(guarded);
    adx_guard_report();

    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail;
}
