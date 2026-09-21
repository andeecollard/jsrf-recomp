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
 * unmatched unlock takes the guard for its own body only and is counted.
 *
 * BOUNDED, because a lock that is never unlocked would otherwise wedge every
 * other guest thread with no diagnosis. After adx_guard_timeout_ms() the
 * waiter takes the guard anyway and counts a steal: that is a return to the
 * unserialised behaviour for one region, which is what we had before this
 * file, and the report names it rather than the run hanging silently. */

struct adx_guard_stats {
    unsigned long locks;             /* guest LOCK calls under the guard */
    unsigned long unlocks_matched;   /* guest UNLOCK that owned the guard */
    unsigned long unlocks_unmatched; /* ...that did not: no matching LOCK */
    unsigned long contended;         /* acquisitions that had to wait */
    unsigned long steals;            /* waits that timed out and took it */
    unsigned long stolen_from;       /* releases by a thread already stolen from */
    unsigned      max_depth;         /* deepest nesting reached */
    unsigned      depth;             /* nesting held right now */
    unsigned long owner;             /* id of the holder, 0 when free */
};

int  adx_guard_on(void);            /* RECOMP_ADX_SERIALIZE, value-aware */
unsigned adx_guard_timeout_ms(void);/* RECOMP_ADX_LOCK_TIMEOUT_MS, default 5000 */

/* Bracket the guest LOCK body. enter() returns holding the guard and KEEPS it
 * held after the body finishes -- there is no matching leave here on purpose;
 * the release belongs to the guest's unlock. */
void adx_guard_lock_enter(void);

/* Bracket the guest UNLOCK body. enter() returns 1 when this thread already
 * held the guard (a matched unlock) and 0 when it did not (unmatched, guard
 * taken for the body alone). Either way leave() drops exactly one level, so
 * the caller does not have to know which it was; the return value is for the
 * report and the tests. */
int  adx_guard_unlock_enter(void);
void adx_guard_unlock_leave(void);

void adx_guard_read_stats(struct adx_guard_stats *out);
void adx_guard_report(void);        /* one [ADX-GUARD] line on stderr */

/* Tests only: forget every thread's nesting and free the guard. */
void adx_guard_reset_for_test(void);

#endif /* JSRF_ADX_GUARD_H */
