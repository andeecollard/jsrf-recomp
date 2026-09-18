/* WHICH GUEST THREAD STOPPED?
 *
 * The 18 Sep 2026 player freeze produced a complete ordinal census -- eleven
 * ordinals at exactly zero across 165,000 subsequent calls -- and no way at all
 * to attribute those calls to a thread. `esp=`, sampled once per periodic
 * report, was the only fingerprint available, and at one sample a second it
 * could not separate "this thread stopped" from "this thread slowed down
 * fourfold". It did not separate them: the quiet thread's last sample landed
 * 26 s after the freeze.
 *
 * So the census buckets on g_fs_base, the guest TIB, which is RECOMP_TLS and
 * therefore already per-thread.
 *
 * WHAT THIS TEST IS FOR. The failure mode of a distinct-counter is that it
 * silently fails to distinguish, and then reads 1 -- which is both the
 * reassuring answer and indistinguishable from the true one. That is the same
 * shape as the stale "default on" comment and the v1 dsound probe that
 * reported "already instrumented"; this tree has now been bitten by an
 * instrument that reads like a working instrument three times.
 *
 * NEGATIVE CONTROL: make the slot search ignore fs_base and always take slot 0
 * and "two threads: distinct" fails. Make the fs_base==0 fold a `return` and
 * "no-TIB thread is still a thread" fails. The test is not vacuous.
 */
#include <stdint.h>
#include <stdio.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

/* Mirrors KTHREAD_MAX in src/kernel/kernel_bridge.c. If that grows this fails
 * loudly at the overflow case rather than drifting quietly out of date. */
#define EXPECT_MAX 16u

extern void xbox_bridge_note_thread_call(uint32_t fs_base, unsigned int ordinal,
                                         unsigned long tick);
extern int  xbox_bridge_kthread_slot(unsigned int i, uint32_t *fs_base,
                                     unsigned long *calls,
                                     unsigned int *last_ordinal,
                                     unsigned long *last_tick);
extern unsigned long g_kthread_distinct;
extern unsigned long g_kthread_over;

static int fails;

static void check(const char *what, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("  ok   %-46s %lu\n", what, got);
    } else {
        printf("  FAIL %-46s got %lu, want %lu\n", what, got, want);
        ++fails;
    }
}

static int find_slot(uint32_t fs, unsigned long *calls, unsigned int *ord)
{
    unsigned int i;
    uint32_t got;
    for (i = 0; i < EXPECT_MAX; ++i)
        if (xbox_bridge_kthread_slot(i, &got, calls, ord, NULL) && got == fs)
            return 1;
    return 0;
}

int main(void)
{
    unsigned int i, ord = 0;
    unsigned long calls = 0;

    /* Not-measured state. distinct=0 AND over=0 is what "never armed" looks
     * like, and is why the report says OFF out loud rather than printing an
     * empty list. */
    check("fresh: distinct", g_kthread_distinct, 0);
    check("fresh: over", g_kthread_over, 0);

    /* One thread seen repeatedly must count once, not once per call. */
    for (i = 0; i < 100; ++i)
        xbox_bridge_note_thread_call(0x00081000u, 204u, 1000u + i);
    check("one thread x100: distinct", g_kthread_distinct, 1);
    check("one thread x100: found", find_slot(0x00081000u, &calls, &ord), 1);
    check("one thread x100: calls", calls, 100);
    check("one thread x100: last ordinal", ord, 204);

    /* A second thread is the whole point: this is what esp= could not do. */
    xbox_bridge_note_thread_call(0x00082000u, 129u, 2000u);
    check("two threads: distinct", g_kthread_distinct, 2);
    check("two threads: second found", find_slot(0x00082000u, &calls, &ord), 1);
    check("two threads: second calls", calls, 1);
    check("two threads: second ordinal", ord, 129);
    /* and the first is undisturbed */
    check("two threads: first still 100", find_slot(0x00081000u, &calls, &ord) ? calls : 0, 100);

    /* A thread with no TIB is still a thread. Dropping it would under-count
     * precisely the odd thread most worth seeing. */
    xbox_bridge_note_thread_call(0u, 15u, 3000u);
    check("no-TIB thread is still a thread", g_kthread_distinct, 3);
    check("no-TIB folded to sentinel", find_slot(0xFFFFFFFFu, &calls, &ord), 1);

    /* Overflow must be visible, not silent. Fill the remaining slots, then
     * push one more. */
    for (i = 0; i < EXPECT_MAX; ++i)
        xbox_bridge_note_thread_call(0x00090000u + i * 0x1000u, 1u, 4000u);
    check("saturated: distinct caps at max", g_kthread_distinct, EXPECT_MAX);
    check("saturated: overflow counted", g_kthread_over > 0, 1);

    /* A known thread still lands after saturation -- the table is full, not
     * broken. */
    calls = 0;
    xbox_bridge_note_thread_call(0x00081000u, 199u, 5000u);
    check("known thread after saturation", find_slot(0x00081000u, &calls, &ord) ? calls : 0, 101);
    check("  and its ordinal updated", ord, 199);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
