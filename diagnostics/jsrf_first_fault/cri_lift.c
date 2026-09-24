/* The CRI ADX idle spinner, answered by the host (24 Sep 2026).
 *
 * sub_0013B180 is one of the four threads CRI's ADX setup (sub_0013B330)
 * creates: at priority 2 it does nothing but increment 0x25EFA8 until
 * 0x25EFC0 is set, then ExitThread. On the Xbox that is harmless -- the
 * scheduler only runs it when every other thread is idle, and the ADX lock
 * suspends it again. Here guest threads run on real host cores, and thread
 * suspension is cooperative (it lands at kernel calls), so a loop with no
 * kernel call can never be suspended: a 24 Sep profile of the tutorial found
 * it burning a whole core (3,858 samples in 12 s). Nothing reads the counter
 * (xrefs: only the spinner's own two instructions touch 0x25EFA8).
 *
 * The host body keeps the contract -- the counter still moves, the exit flag
 * still ends the thread through the same ExitThread call -- but sleeps between
 * increments and reaches the suspend point, so the ADX lock's
 * SuspendThread/ResumeThread pair now actually parks it.
 * RECOMP_CRI_SPINNER_LIFT=0 runs the original. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void w32_thread_suspend_point(void);

static int g_on = -1;
int crl_on(void)
{
    if (g_on < 0) {
        const char *v = getenv("RECOMP_CRI_SPINNER_LIFT");
        g_on = !(v && strcmp(v, "0") == 0);
        fprintf(stderr, "[CRI-LIFT] idle spinner %s\n", g_on ? "on the host (sleeps, suspendable)" : "original");
    }
    return g_on;
}

void crl_sub_0013B180(void)
{
    while (!MEM32(0x0025EFC0u)) {
        MEM32(0x0025EFA8u) += 1u;
        w32_thread_suspend_point();
        usleep(1000);
    }
    /* The original's exit path: push 0xF0000001; [0x25EFC4] = 1; call
     * 0x147E4E (XAPI ExitThread), which does not return. */
    esp -= 4u; MEM32(esp) = 0xF0000001u;
    MEM32(0x0025EFC4u) = 1u;
    esp -= 4u; MEM32(esp) = 0x0013B1B8u;
    sub_00147E4E();
}
