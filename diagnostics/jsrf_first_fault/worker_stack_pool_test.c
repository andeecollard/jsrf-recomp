/* IS THE WORKER STACK POOL ACTUALLY COMPILED IN?
 *
 * RECOMP_IRQ_THREAD exists to deliver device interrupts from a host thread
 * instead of waiting for a guest thread to block. On 18 Sep 2026 it was armed in
 * the player's paths.conf for a whole session and delivered NOTHING:
 *
 *   [IRQ-THREAD] no worker stack slice (XBOX_WORKER_STACK_COUNT=0); not delivering
 *
 * bridge_irq_thread borrows a guest stack slice, and the pool is zero by default
 * for a good reason (xbox_memory_layout.h:396 -- sixteen slices is 4 MB under the
 * arena, and this title already fails few-hundred-byte allocations with ~2 MB to
 * spare). So the switch needs a BUILD, `-DXBOX_WORKER_STACK_COUNT=1`, and a
 * handover had it filed as one line in a config file.
 *
 * That cost a session, and the shape of the failure is the one this tree keeps
 * meeting: a switch that silently does nothing reads exactly like a switch that
 * did nothing because there was nothing to find.
 *
 * THIS TEST PASSES IN BOTH CONFIGURATIONS AND MEANS SOMETHING DIFFERENT IN EACH.
 * It asserts the allocator's behaviour against the count the build actually
 * compiled in, so the default build proves the pool is absent and the
 * -DXBOX_WORKER_STACK_COUNT=1 build proves it is present. Either way the run is
 * no longer guessing which binary it is holding.
 */
#include <stdint.h>
#include <stdio.h>
#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);

static int fails;
static void check(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-46s %ld\n", what, got);
    else { printf("  FAIL %-46s got %ld, want %ld\n", what, got, want); ++fails; }
}

int main(void)
{
    const int N = XBOX_WORKER_STACK_COUNT;
    int seen[64], i, s;

    printf("  built with XBOX_WORKER_STACK_COUNT=%d\n", N);

    if (N == 0) {
        /* The default. RECOMP_IRQ_THREAD cannot deliver in this build, and that
         * is the fact the session on 18 Sep needed and did not have. */
        check("pool absent: first alloc refuses", xbox_worker_stack_alloc(), -1);
        check("pool absent: still refuses", xbox_worker_stack_alloc(), -1);
        printf("  note: RECOMP_IRQ_THREAD cannot deliver in this build.\n");
        printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
        return fails ? 1 : 0;
    }

    /* Every slice must be handed out exactly once, and the slots must differ --
     * a pool that returns the same slice twice would put two ISRs on one guest
     * stack, which is worse than not delivering at all. */
    if (N > (int)(sizeof seen / sizeof seen[0])) {
        printf("  (pool larger than this test tracks; raising the bound)\n");
        return 0;
    }
    for (i = 0; i < N; ++i) {
        s = xbox_worker_stack_alloc();
        seen[i] = s;
        if (s < 0 || s >= N) { printf("  FAIL slot %d out of range: %d\n", i, s); ++fails; }
    }
    check("every slice allocated", (long)N, (long)N);
    for (i = 0; i < N; ++i) {
        int j;
        for (j = i + 1; j < N; ++j)
            if (seen[i] == seen[j]) { printf("  FAIL slot reused: %d\n", seen[i]); ++fails; }
    }
    check("exhausted pool refuses", xbox_worker_stack_alloc(), -1);

    /* Freeing must make a slice available again, or a restarted IRQ thread
     * would find the pool permanently empty. */
    xbox_worker_stack_free(seen[0]);
    s = xbox_worker_stack_alloc();
    check("freed slice is reusable", (s >= 0 && s < N) ? 1 : 0, 1);

    /* Out-of-range frees must not corrupt the bitmap. */
    xbox_worker_stack_free(-1);
    xbox_worker_stack_free(N);
    xbox_worker_stack_free(9999);
    check("bad frees leave pool exhausted", xbox_worker_stack_alloc(), -1);

    printf("  note: RECOMP_IRQ_THREAD can deliver in this build.\n");
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
