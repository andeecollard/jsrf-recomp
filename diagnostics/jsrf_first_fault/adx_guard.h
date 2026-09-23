#ifndef JSRF_ADX_GUARD_H
#define JSRF_ADX_GUARD_H

/* THE LOCK CRI'S ADX LOCK ACTUALLY IS.
 *
 * sub_0013B0A0 / sub_0013B0E0 are a lock/unlock pair whose mutual exclusion
 * was priority elevation on a single CPU: the caller raises itself to base 16
 * and no other guest thread can run until it lowers itself again. The refcount
 * at 0x0025EFA0 and the one saved-priority slot at 0x0027D0F8 are safe only
 * under that guarantee.
 *
 * SERIALISING THE TWO BODIES AGAINST EACH OTHER DOES NOT RESTORE IT, and the
 * first version of this fix did exactly that. The poisoning interleave is five
 * complete, already-serialised calls:
 *
 *     A locks    count 0->1, A raised to 16, A's real priority saved
 *     B locks    count 1->2, the `jne` skips, B untouched
 *     A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
 *     B unlocks  count 1->0, restores A's saved value onto B
 *     A locks    A is already at 16, so it saves 15.  POISONED.
 *
 * Nothing above requires two bodies to overlap in time. What it requires is B
 * entering the region while A is inside it, which is precisely what priority
 * elevation prevented and what a per-body mutex permits.
 *
 * So the guard spans the GUEST critical section: taken in the lock, held
 * across the caller's own work, released by the matching unlock. That is the
 * hardware's guarantee, and under it the interleave above cannot be written
 * down -- B's lock blocks until A's unlock.
 *
 * RECURSIVE, because the count is a recursion count and the guest does nest.
 * OWNER-TRACKED, because the guest also issues unlocks it never matched with a
 * lock: sub_001437B0's I/O guard calls the registered unlock on every spin
 * pass. Releasing on those would hand another thread's region away, so an
 * unmatched unlock takes the guard for its own body only -- and only if it is
 * free. See AN UNMATCHED UNLOCK NEVER STEALS below for the case where it is
 * not.
 *
 * BOUNDED ON THE LOCK PATH ONLY, because a lock that is never unlocked would
 * otherwise wedge every other guest thread with no diagnosis. After
 * adx_guard_timeout_ms() a waiting LOCK takes the guard anyway and counts a
 * steal: that is a return to the unserialised behaviour for one region, which
 * is what we had before this file, and the report names it rather than the
 * run hanging silently.
 *
 * AN UNMATCHED UNLOCK NEVER STEALS, and the first version of this file let it.
 * Player session 10 on 21 Sep 2026 ran at 0.4 fps for its last 171 s, and the
 * shape of it was one 5000 ms frame per five-second window -- the steal
 * timeout, not slow work:
 *
 *     A locks             count 0->1, A raised to 16, A's real priority (1)
 *                         saved. A holds the guard across its own file I/O.
 *     B unlocks UNMATCHED sub_001437B0's spin pass. It waits 5000 ms on A,
 *                         STEALS, and runs: count 1->0, restoring 1 onto B.
 *     A locks (nested)    the count is 0 again so the `jne` no longer skips,
 *                         and A saves its own priority -- but A is still at
 *                         16, so GetThreadPriority reads 15.  POISONED.
 *
 * From there every unlock that reaches zero restores 15 -> base 16, nothing
 * can lower itself, and CRI's I/O guard spins for ever. The log showed each
 * step: saved_priority 1 -> 15 at the cliff and never back, locks one ahead of
 * unlocks for good, and stolen_from=0 because A never returned.
 *
 * The steal is what breaks the guarantee, so the unlock path does not have
 * one. On hardware B's spin pass could not have run at all while A was
 * elevated -- that is what elevation MEANT -- so the faithful answer for a
 * contended unmatched unlock is not to wait and not to take it, but to SKIP
 * THE PASS: the caller drops the body and CRI's spin comes round again. It
 * cannot deadlock, because it never waits.
 *
 * THE LOCK PATH HAS THE SAME HOLE, and adx_guard_test's own interleave finds
 * it the moment RECOMP_ADX_LOCK_TIMEOUT_MS is short enough for the test's
 * patience window: B's LOCK steals, B enters while A is inside, and step 5
 * saves 15. The default of 5000 ms was hiding it, not preventing it. So the
 * steal is now OFF by default on that path too -- a waiter waits, and says
 * every timeout period who it is waiting for.
 *
 * That trades a livelock for a possible deadlock, and it is the right trade
 * on the evidence: session 10 logged 85 contended acquisitions in its healthy
 * 996 s against 206,583 locks, and NOT ONE of them reached even a second, let
 * alone five. Contention here is rare and short. Every steal in that run
 * happened after the state was already poisoned. A steal has never once been
 * observed to rescue a run; it has been observed to wreck one.
 *
 * RECOMP_ADX_LOCK_STEAL=1 restores the old behaviour for an A/B. */

struct adx_guard_stats {
    unsigned long locks;             /* guest LOCK calls under the guard */
    unsigned long unlocks_matched;   /* guest UNLOCK that owned the guard */
    unsigned long unlocks_unmatched; /* ...that did not: no matching LOCK */
    unsigned long unlocks_skipped;   /* ...and were refused the body entirely,
                                      * because another thread was inside */
    unsigned long contended;         /* acquisitions that had to wait */
    unsigned long steals;            /* waits that timed out and took it
                                      * anyway -- RECOMP_ADX_LOCK_STEAL only */
    unsigned long stalls;            /* timeout periods spent still waiting */
    unsigned long stolen_from;       /* releases by a thread already stolen from */
    unsigned      max_depth;         /* deepest nesting reached */
    unsigned      depth;             /* nesting held right now */
    unsigned long owner;             /* id of the holder, 0 when free */
};

int  adx_guard_on(void);            /* RECOMP_ADX_SERIALIZE, value-aware */
unsigned adx_guard_timeout_ms(void);/* RECOMP_ADX_LOCK_TIMEOUT_MS, default 5000 */
int  adx_guard_steal_on(void);      /* RECOMP_ADX_LOCK_STEAL, default OFF */

/* Bracket the guest LOCK body. enter() returns holding the guard and KEEPS it
 * held after the body finishes -- there is no matching leave here on purpose;
 * the release belongs to the guest's unlock. */
void adx_guard_lock_enter(void);

/* Bracket the guest UNLOCK body:
 *
 *     1  this thread already held the guard -- a matched unlock. Run the body.
 *     0  it did not, and the guard was free, so it has been taken for this
 *        body alone. Run the body.
 *    -1  it did not, and ANOTHER THREAD IS INSIDE THE REGION. Do NOT run the
 *        body, and do NOT call leave() -- nothing was taken. Running it here
 *        is what poisons 0x0027D0F8; see the top of this file.
 *
 * On 1 and 0, leave() drops exactly one level, so the caller does not have to
 * know which it was. On -1 the caller returns without touching guest state. */
int  adx_guard_unlock_enter(void);
void adx_guard_unlock_leave(void);

void adx_guard_read_stats(struct adx_guard_stats *out);
void adx_guard_report(void);        /* one [ADX-GUARD] line on stderr */

/* Tests only: forget every thread's nesting and free the guard. */
void adx_guard_reset_for_test(void);

#endif /* JSRF_ADX_GUARD_H */
