/* The crash report has said `sub_001A2E2E +0x670` for weeks and that address
 * meant nothing to anyone. Microsoft's own symbol tables name 162 functions in
 * that section, so the report can say what the code IS.
 *
 * The risk being tested is mis-parsing, not lookup: a recompiled symbol is
 * `sub_<GUESTVA>` and Mach-O may prefix an underscore, and there are other
 * symbols in the binary that start with "sub" and are not that. A parser that
 * accepts them would attach a confident wrong name to a crash report, which is
 * worse than printing the address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "guest_names.h"

static int fails;
static void ok(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-44s %ld\n", what, got);
    else { printf("  FAIL %-44s got %ld, want %ld\n", what, got, want); ++fails; }
}

int main(void)
{
    uint32_t va = 0;
    char path[512];
    FILE *f;
    const char *n;

    /* --- the parser --- */
    ok("plain sub_ parses",        recomp_guest_va_from_symbol("sub_001A2E2E", &va), 1);
    ok("  ...to the right VA",     va, 0x1A2E2E);
    ok("leading underscore parses", recomp_guest_va_from_symbol("_sub_001A216B", &va), 1);
    ok("  ...to the right VA",     va, 0x1A216B);
    ok("rejects a near-miss name", recomp_guest_va_from_symbol("subsystem_init", &va), 0);
    ok("rejects sub_ with junk",   recomp_guest_va_from_symbol("sub_001A2E2E_cold", &va), 0);
    ok("rejects empty sub_",       recomp_guest_va_from_symbol("sub_", &va), 0);
    ok("rejects NULL",             recomp_guest_va_from_symbol(NULL, &va), 0);

    /* --- unloaded: must be silent and empty, not crash --- */
    ok("unloaded: count",  (long)recomp_guest_names_count(), 0);
    ok("unloaded: lookup", recomp_guest_name(0x1A216B) ? 1 : 0, 0);

    /* --- loaded --- */
    snprintf(path, sizeof path, "%s/guest_names_test.%d.tsv",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    f = fopen(path, "w");
    if (!f) { printf("  FAIL could not write %s\n", path); return 1; }
    fputs("# comment line, ignored\n", f);
    fputs("1A216B\t?ServiceDeferredCommandsHigh@CMcpxAPU@DirectSound@@IAEXXZ\n", f);
    fputs("1A200D\tsome_other_name\n", f);
    fputs("garbage with no tab\n", f);
    fputs("1A20E4\t\n", f);                 /* empty name: must be skipped */
    fclose(f);
    setenv("RECOMP_GUEST_NAMES", path, 1);

    ok("loads the well-formed lines", (long)recomp_guest_names_load(), 2);
    ok("count matches",               (long)recomp_guest_names_count(), 2);
    n = recomp_guest_name(0x1A216B);
    ok("exact hit", n && strstr(n, "ServiceDeferredCommandsHigh") ? 1 : 0, 1);
    ok("other exact hit", recomp_guest_name(0x1A200D) ? 1 : 0, 1);
    ok("empty-name line skipped", recomp_guest_name(0x1A20E4) ? 1 : 0, 0);
    /* A near miss must NOT resolve -- naming the wrong function in a crash
     * report is the failure this whole table could introduce. */
    ok("no fuzzy match below", recomp_guest_name(0x1A216A) ? 1 : 0, 0);
    ok("no fuzzy match above", recomp_guest_name(0x1A216C) ? 1 : 0, 0);
    ok("second load is a no-op", (long)recomp_guest_names_load(), 2);

    remove(path);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
