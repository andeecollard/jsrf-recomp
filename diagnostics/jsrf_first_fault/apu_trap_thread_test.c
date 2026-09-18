/* WHICH GUEST THREAD SUBMITS APU METHODS?
 *
 * RECOMP_KERNEL_THREADS settled that no guest thread dies at the APU freeze:
 * four alive with near-constant call rates 1,250 s afterwards, a fifth dormant
 * since t=0. It could not say which thread FEEDS the APU, because APU
 * submission is not a kernel call -- it is an MMIO store into the trapped
 * aperture, which is why [MCPX-TRAP] vp equals guest_methods exactly.
 *
 * This census counts those faults per guest TIB. The counter runs inside a
 * SIGSEGV/SIGBUS handler, so the things this test pins down are as much about
 * safety as about arithmetic:
 *
 *   - the gate must be resolved EAGERLY. xbox_apu_trap_threads_arm() is called
 *     from xbox_McpxTrapInstall before any fault can arrive, because
 *     recomp_switch_on() reads the environment and getenv is not
 *     async-signal-safe. Until it is armed, note_thread must do NOTHING -- an
 *     unarmed counter that still counted would be a lazily-resolved gate by
 *     another name.
 *   - no timestamps, so "stopped" is read from the periodic report as a count
 *     that stops growing. Nothing here should need a clock.
 *
 * NEGATIVE CONTROLS: make the slot search ignore fs_base and "two threads:
 * distinct" fails; drop the fs_base==0 fold and "no-TIB thread" fails; make
 * note_thread count while unarmed and "unarmed: counts nothing" fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EXPECT_MAX 8u   /* mirrors APU_TH_MAX in src/kernel/xbox_memory_layout.c */

extern void xbox_apu_trap_note_thread(uint32_t fs_base, int is_vp);
extern void xbox_apu_trap_threads_arm(void);
extern int  xbox_apu_trap_thread_slot(unsigned int i, uint32_t *fs_base,
                                      unsigned long *apu, unsigned long *vp);
extern unsigned long g_apu_th_distinct;
extern unsigned long g_apu_th_over;

static int fails;
static void check(const char *what, unsigned long got, unsigned long want)
{
    if (got == want) printf("  ok   %-44s %lu\n", what, got);
    else { printf("  FAIL %-44s got %lu, want %lu\n", what, got, want); ++fails; }
}
static int find(uint32_t fs, unsigned long *apu, unsigned long *vp)
{
    unsigned int i; uint32_t got;
    for (i = 0; i < EXPECT_MAX; ++i)
        if (xbox_apu_trap_thread_slot(i, &got, apu, vp) && got == fs) return 1;
    return 0;
}

int main(void)
{
    unsigned int i;
    unsigned long apu = 0, vp = 0;

    /* UNARMED. The gate has not been resolved, so nothing may be recorded --
     * and crucially note_thread must not resolve it itself. */
    for (i = 0; i < 50; ++i) xbox_apu_trap_note_thread(0x00982000u, 1);
    check("unarmed: distinct", g_apu_th_distinct, 0);
    check("unarmed: counts nothing", g_apu_th_over, 0);

    /* Arm the way the trap installer does, outside any handler. */
    setenv("RECOMP_APU_TRAP_THREADS", "1", 1);
    xbox_apu_trap_threads_arm();

    /* One submitting thread, vp and non-vp writes distinguished. */
    for (i = 0; i < 100; ++i) xbox_apu_trap_note_thread(0x00982000u, 1);
    for (i = 0; i < 40;  ++i) xbox_apu_trap_note_thread(0x00982000u, 0);
    check("one thread: distinct", g_apu_th_distinct, 1);
    check("one thread: found", find(0x00982000u, &apu, &vp), 1);
    check("one thread: apu total", apu, 140);
    check("one thread: vp subset", vp, 100);

    /* A second submitter is the question this instrument exists to answer. */
    xbox_apu_trap_note_thread(0x00001000u, 1);
    check("two threads: distinct", g_apu_th_distinct, 2);
    check("two threads: second found", find(0x00001000u, &apu, &vp), 1);
    check("two threads: second vp", vp, 1);
    check("two threads: first undisturbed", find(0x00982000u, &apu, &vp) ? apu : 0, 140);

    /* A thread with no TIB is still a thread. */
    xbox_apu_trap_note_thread(0u, 0);
    check("no-TIB thread counted", g_apu_th_distinct, 3);
    check("no-TIB folded to sentinel", find(0xFFFFFFFFu, &apu, &vp), 1);

    /* Saturation is visible, not silent. */
    for (i = 0; i < EXPECT_MAX; ++i)
        xbox_apu_trap_note_thread(0x00A00000u + i * 0x1000u, 0);
    check("saturated: distinct caps", g_apu_th_distinct, EXPECT_MAX);
    check("saturated: overflow counted", g_apu_th_over > 0, 1);

    /* A known thread still lands once the table is full. */
    xbox_apu_trap_note_thread(0x00982000u, 1);
    check("known thread after saturation", find(0x00982000u, &apu, &vp) ? vp : 0, 101);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
