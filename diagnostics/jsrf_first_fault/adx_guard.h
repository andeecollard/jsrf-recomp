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
    unsigned long block_releases;    /* holder blocked in a kernel wait: guard let go */
    unsigned long block_reacquires_contended; /* ...and on waking had to wait for it */
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

/* THE GUARD IS PRIORITY, AND PRIORITY ENDS WHEN THE HOLDER BLOCKS.
 *
 * Priority elevation excludes other threads only while the elevated thread is
 * RUNNING. When it blocks -- a kernel wait that is not already satisfied --
 * the single CPU goes to whoever is runnable, and a normal-priority thread may
 * then enter the region: its lock takes the `jne` path and changes no priority.
 * The guard modelled elevation as a lock held from lock to unlock, so a holder
 * that waited inside the region kept every other thread out for the whole
 * wait. Player session of 23 Sep 2026 (17:13, 820 s): the ADX thread held the
 * guard across a loop of D3DDevice_BlockUntilVerticalBlank waits
 * (sub_0013B1C0), the main thread waited for the guard in sub_0013B0A0, and
 * the picture froze at t=780 s with 8 stalls reported.
 *
 * So the kernel's blocking waits call these around the part that actually
 * blocks: begin releases the guard completely if this thread holds it and
 * returns the nesting to restore (0 if it held nothing); end re-takes it --
 * waiting like any other entrant if someone else is inside now -- and restores
 * the nesting. Installed as the kernel's blocking-wait hooks by main.c. */
unsigned adx_guard_block_begin(void);
void     adx_guard_block_end(unsigned saved_depth);
void adx_guard_report(void);        /* one [ADX-GUARD] line on stderr */

/* ── WHOSE PRIORITY THE ONE SLOT HOLDS (G66) ─────────────────────────────
 *
 * 0x0027D0F8 is ONE slot, written by the lock that takes the count 0 -> 1 and
 * read back by the unlock that takes it 1 -> 0 -- onto whichever thread made
 * that unlock. The block-release rule above lets a second thread M into the
 * region while the raiser A waits in a kernel wait, and then the unlock that
 * reaches zero can be M's:
 *
 *     A locks    count 0->1, slot := A's real priority (1), A raised to 16
 *     A blocks   the guard lets go (THE GUARD IS PRIORITY)
 *     M locks    count 1->2, the `jne` skips, M untouched
 *     A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
 *     M unlocks  count 1->0, restores A's 1 ONTO M
 *
 * That is the "A16 M1" run in every poisoned KeSetBasePriority trace of G66
 * (s4m96 s8m13 s6m30 s6m61): A is never lowered, every later A lock saves 15,
 * the 15 migrates to main, and CRI's drop-all-nesting loop at sub_001437B0
 * (`while GetThreadPriority(self) == 15: unlock`) spins with unmatched
 * unlocks driving the count negative.
 *
 * PER-THREAD RESTORE (RECOMP_ADX_PER_THREAD_RESTORE, default ON). The lock
 * still writes the guest's slot exactly as the guest does; the ledger below
 * ALSO records, per raising thread, its own pre-raise priority. When the
 * count reaches 0 on a thread that is not the raiser, the unlocking thread's
 * priority is left alone and the raiser is OWED its own saved priority. The
 * raiser collects it at its next entry to the lock or the unlock, on its own
 * thread, through the guest's own SetThreadPriority(NtCurrentThread) -- the
 * only priority path this runtime can aim at a thread, since a thread-id
 * token only resolves on the thread it names (kernel_bridge.c). Off, the
 * unlock restores the slot onto whoever unlocks, as the guest does.
 *
 * UNMATCHED-SAFE (RECOMP_ADX_UNMATCHED_SAFE, default ON). An unlock that finds
 * the count already <= 0 never decrements it: on hardware no unlock can run
 * there, and a negative count turns both routines into permanent no-ops (both
 * open with a `jne` on it). If the caller holds no lock and still reads
 * GetThreadPriority == 15, its own last pre-raise priority is restored (0,
 * THREAD_PRIORITY_NORMAL, if it never raised -- counted as a fallback) so
 * sub_001437B0's loop sees something other than 15 and exits.
 *
 * Both are decisions only: the bodies in jsrf_manual_overrides.c ask, and do
 * the guest calls themselves. adx_guard_test drives the exact interleave
 * above through the same calls. */
int  adx_per_thread_restore_on(void);   /* RECOMP_ADX_PER_THREAD_RESTORE, default ON */
int  adx_unmatched_safe_on(void);       /* RECOMP_ADX_UNMATCHED_SAFE, default ON */

/* The lock body, after its `jne`: raised=1 when this lock took the count 0->1
 * and saved `pre_raise` (what GetThreadPriority returned) into the slot. */
void adx_prio_note_lock(int raised, int pre_raise);
/* The unlock body, once admitted: 1 if this thread holds a lock by the
 * ledger's count (and drops one level of it), 0 if it holds none. */
int  adx_prio_note_unlock(void);
/* The unlock took the count to 0. 1: restore the slot onto THIS thread, as
 * the guest does. 0: do not -- the raiser was another thread and is now owed
 * its own priority. Always 1 with the switch off. */
int  adx_prio_restore_here(void);
/* At entry to either routine: 1 and *prio if this thread is owed a restore
 * (consumed). Never 1 with the switch off. */
int  adx_prio_take_owed(int *prio);
/* The unmatched-safe rescue: this thread's own last pre-raise priority. 1 if
 * it ever raised, 0 (and *prio = 0, NORMAL) if not. Counts a rescue. */
int  adx_prio_rescue(int *prio);
void adx_prio_note_clamp(void);         /* an unlock at count <= 0 was not applied */

/* ── THE TRACE (RECOMP_ADX_TRACE, default OFF) ───────────────────────────
 *
 * A ring of the last 256 events -- lock, unlock, block_begin, block_end, plus
 * the owed-restore and clamp decisions above -- each with the host thread id,
 * the guest return address MEM32(esp) at entry, the refcount before and after,
 * the guard nesting (this thread / holder), the unlock admission result and
 * the priority handed to SetThreadPriority (or none). Dumped as [ADX-TRACE]
 * lines on stderr ONCE per reason:
 *
 *     leak       locks - matched unlocks - live nesting (held + parked by a
 *                blocking wait) moved off zero: the ~25% lock leak G66 could
 *                not explain, with the events that made it
 *     unmatched  the first unlock from a thread that holds nothing
 *     skipped    the first unlock refused because another thread was inside
 *     cross      the first count-to-zero on a thread that was not the raiser
 *     clamp      the first unlock at count <= 0
 *     drift      the guest refcount first disagreed with the ledger's
 *                sum of held locks
 */
enum adx_trace_type {
    ADX_EV_LOCK = 1, ADX_EV_UNLOCK, ADX_EV_BLOCK_BEGIN, ADX_EV_BLOCK_END,
    ADX_EV_OWED, ADX_EV_CLAMP
};
#define ADX_PRIO_NONE (-1000)           /* "no SetThreadPriority in this event" */
#define ADX_COUNT_NA  (-2147483647 - 1) /* the count was not readable here */
int  adx_trace_on(void);
/* Where block_begin/end read the guest refcount from; set by the overrides. */
void adx_trace_set_count_source(int (*read_count)(void));
void adx_trace_note(int type, unsigned ra, int count_before, int count_after,
                    int unlock_result, int prio_set);
void adx_trace_dump(const char *reason);  /* unconditional; tests and reports */
/* Which reasons have dumped so far, one bit each in the order listed above
 * (leak 1, unmatched 2, skipped 4, cross 8, clamp 16, drift 32). Tests. */
unsigned adx_trace_fired(void);

/* Tests only: forget every thread's nesting and free the guard. */
void adx_guard_reset_for_test(void);
void adx_prio_reset_for_test(void);  /* the ledger, owed restores and trace */

#endif /* JSRF_ADX_GUARD_H */
