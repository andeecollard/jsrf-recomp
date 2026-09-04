/* Focused regression coverage for the OHCI root-hub register model. */
#include <stdint.h>
#include <stdio.h>

#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

int main(void)
{
    uint32_t status;

    status = xbox_OhciPortWrite(0x00010001u, 0x00010000u);
    check(status == 0x00000001u,
          "connect-change acknowledgement preserves connection status");

    /* XPP's observed reset write carries stale/reserved high bits. Those may
     * acknowledge old changes, but cannot acknowledge the reset-completion
     * change generated after the write. */
    status = xbox_OhciPortWrite(0x00000001u, 0x009E0010u);
    check(status == 0x00100003u,
          "instant reset reports enabled plus reset-status-change");

    status = xbox_OhciPortWrite(status, 0x00100000u);
    check(status == 0x00000003u,
          "reset-status-change remains write-1-to-clear");

    status = xbox_OhciPortWrite(0x00000000u, 0x00000010u);
    check(status == 0x00000000u,
          "an empty port does not complete a reset");

    if (failures) return 1;
    puts("ohci_register_test: all checks passed");
    return 0;
}
