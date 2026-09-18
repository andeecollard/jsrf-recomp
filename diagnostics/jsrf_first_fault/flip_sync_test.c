/* DOES THE FLIP READ-BACK'S SWITCH ANNOUNCE ITSELF, IN BOTH ARMS?
 *
 * RECOMP_METAL_NO_FLIP_SYNC skips the GPU drain and colour read-back that
 * snapshot_surface() takes at every NV097_FLIP_STALL. That drain is the single
 * largest identified stage in the frame -- measured 18 Sep 2026 as [STAGE]
 * sync = 5.19 ms of a 16.43 ms frame in a player session with no
 * instrumentation, and 11.89 ms of 32.82 ms in a heavier scripted scene -- so
 * the A/B that decides whether direct Metal presentation is worth building
 * hangs off this switch.
 *
 * WHAT THIS TEST IS FOR, AND WHY IT IS NOT ABOUT THE DRAIN. The drain only
 * runs when nv2a_gpu_on() is true, which needs RECOMP_METAL and a live device;
 * this harness has neither, so `taken` and `skipped` are both 0 here by
 * construction and asserting on them would be asserting on the harness. What
 * CAN be pinned without a GPU is the thing that has actually gone wrong before:
 *
 *   1. THE TOKEN IS VISIBLE TO ab_score.py IN BOTH ARMS. defer_swap's whole
 *      A/B was scored with its identical-arms VOID check silently skipped,
 *      because the token it printed was invisible to the regex of the day.
 *      ab_score.py matches r"\((\w+ (?:on|OFF))\)", so the line must carry
 *      literally "(no_flip_sync on)" or "(no_flip_sync OFF)" -- and it must
 *      carry it in the OFF arm too, or the control arm has nothing to differ
 *      from.
 *
 *   2. THE GRAMMAR IS recomp_switch_on, NOT A BARE getenv. RECOMP_IRQ_THREAD
 *      is a bare getenv, so RECOMP_IRQ_THREAD=0 turns it ON, and the comment
 *      in the player's paths.conf has to warn about it. =0 must mean OFF here.
 *      This is the arm that catches a hand-rolled gate being reintroduced.
 *
 *   3. THE LINE IS UNCONDITIONAL. A counter that prints only when it fired
 *      cannot size an A/B before the A/B is run, which is the entire reason
 *      this one is counted in both arms.
 *
 * NEGATIVE CONTROL, RUN, NOT IMAGINED: change the report to print the token
 * only when the switch is on and the OFF arm below fails; change the gate to a
 * bare getenv and the "=0 means OFF" arm fails. Either way the suite goes red.
 */
#define _POSIX_C_SOURCE 200809L
#include "nv2a_regs.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the harness the executor needs, exactly as jsrf_fb_watch_still's ---- */
static uint32_t ram[32768];
static uint8_t gpu_regs[0x800000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
void *xbox_GpuMemoryRange(uint32_t address, size_t bytes) {
    return (uint64_t)address + bytes <= sizeof(ram) ? (uint8_t *)ram + address : NULL;
}
const uint8_t *xbox_Nv2aRegisterMemory(void) { return gpu_regs; }
double xbox_TraceSeconds(void) { return 0.0; }
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size)
{
    (void)xbox_va;
    if (buf && size) snprintf(buf, size, "no heap in this harness");
    return 0;
}
void xbox_FramebufferWindowSet(uint32_t a, uint32_t p) { (void)a; (void)p; }
void xbox_FramebufferWindowStart(void) {}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_report(void);
const void *nv2a_pb_exec_surface(uint32_t *w, uint32_t *h,
                                 uint32_t *pitch, uint32_t *bpp);

#define FRAME_W 64u
#define FRAME_H 48u

static void put(uint32_t m, uint32_t p) { nv2a_pb_exec_method(0, m, p); }

/* The child: three flips through the real method sink, then the report. */
static void child(void)
{
    uint32_t w, h, p, b;
    unsigned i;

    nv2a_pb_exec_surface(&w, &h, &p, &b);      /* the presenter asks */
    put(NV097_SET_SURFACE_CLIP_HORIZONTAL, FRAME_W << 16);
    put(NV097_SET_SURFACE_CLIP_VERTICAL, FRAME_H << 16);
    put(NV097_SET_SURFACE_PITCH, FRAME_W * 4u);
    for (i = 0; i < 3; ++i) {
        put(NV097_SET_SURFACE_COLOR_OFFSET, 0x1000u + i * 0x1000u);
        put(NV097_FLIP_STALL, 0);
    }
    nv2a_pb_exec_report();
    exit(0);
}

/* ---- the parent ---------------------------------------------------------- */
static int fails;
static char out[1 << 20];

static void ok(const char *what, int cond)
{
    if (cond) printf("  ok   %s\n", what);
    else { printf("  FAIL %s\n", what); ++fails; }
}

/* One arm per process: the gate caches its answer in a static. */
static void run(const char *env, const char *exe)
{
    char cmd[2048];
    FILE *p;
    size_t n = 0, r;

    snprintf(cmd, sizeof cmd, "%s \"%s\" child 2>&1", env, exe);
    out[0] = '\0';
    p = popen(cmd, "r");
    if (!p) { printf("  FAIL could not run %s\n", cmd); ++fails; return; }
    while ((r = fread(out + n, 1, sizeof out - 1 - n, p)) > 0) n += r;
    out[n] = '\0';
    pclose(p);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) { child(); return 0; }

    printf("flip-sync switch\n");

    /* The control arm. It must still print, and it must print OFF -- that is
     * what lets one run size the A/B before the A/B is taken. */
    run("", argv[0]);
    ok("unset: [FLIP-SYNC] line is printed",      strstr(out, "[FLIP-SYNC]") != NULL);
    ok("unset: carries the ab_score token OFF",   strstr(out, "(no_flip_sync OFF)") != NULL);
    ok("unset: does not claim to be on",          strstr(out, "(no_flip_sync on)") == NULL);

    /* The grammar arm. recomp_switch_on means =0 is OFF; a bare getenv would
     * read the string "0" as set and turn it ON. */
    run("RECOMP_METAL_NO_FLIP_SYNC=0", argv[0]);
    ok("=0: still OFF (not a bare getenv)",       strstr(out, "(no_flip_sync OFF)") != NULL);

    run("RECOMP_METAL_NO_FLIP_SYNC=1", argv[0]);
    ok("=1: line is printed",                     strstr(out, "[FLIP-SYNC]") != NULL);
    ok("=1: carries the ab_score token on",       strstr(out, "(no_flip_sync on)") != NULL);

    /* The word grammar, because recomp_switch_on accepts it and a hand-rolled
     * replacement typically would not. */
    run("RECOMP_METAL_NO_FLIP_SYNC=yes", argv[0]);
    ok("=yes: word grammar is honoured",          strstr(out, "(no_flip_sync on)") != NULL);

    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
