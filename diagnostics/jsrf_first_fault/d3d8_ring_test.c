/*
 * Does the runtime find the D3D8 device global, and does it fall back safely?
 *
 * `d3d8_ring.c` exists so the push-buffer pump stops being per-title: one
 * symbol, `D3D8__D3D_g_pDevice`, gives the device, and every ring field is a
 * fixed offset inside it. The lookup is therefore the load-bearing part, and
 * it has already been wrong once -- the TSV branch took the whole rest of the
 * line as the symbol name, matched nothing, and fell back to the built-in
 * default. A scratch check called that a PASS because the default happened to
 * be the right answer.
 *
 * So every case here passes a default that is NEVER the expected value, except
 * the two that are testing the fallback itself. A silent fallback cannot look
 * like a successful lookup.
 *
 * One case per process: the resolution is cached on first use, deliberately,
 * because a device global that moved mid-run would be a far worse bug than a
 * stale cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/kernel/d3d8_ring.h"

#define DEVICE_GLOBAL 0x0019DCE0u
#define NEVER_RIGHT   0xDEADBEEFu

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

/* Both shapes carry decoys: a name that CONTAINS the wanted one without ending
 * in it, and a row whose address column is right but whose name is not. */
static const char CLI_DUMP[] =
    "XAPILIB__XInputOpen = 0x001c3ba1\n"
    "D3D8__D3D_g_pDeviceExtraSuffix = 0x00111111\n"
    "D3D8__D3D_g_Stream = 0x0019dce8\n"
    "D3D8__D3D_g_pDevice = 0x0019dce0\n";

static const char TSV_DUMP[] =
    "0x00111111\tfunc\tundefined\t__cdecl\tnotinline\tnofixup\t"
        "D3D8__D3D_g_pDeviceExtraSuffix\tnocomment\n"
    "0x0019dce8\tfunc\tundefined\t__cdecl\tnotinline\tnofixup\t"
        "D3D8__D3D_g_Stream\tnocomment\n"
    "0x0019dce0\tfunc\tundefined\t__cdecl\tnotinline\tnofixup\t"
        "D3D8__D3D_g_pDevice\tnocomment\n";

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "fallback";
    uint32_t expect, got, given = NEVER_RIGHT;
    char path[512];

    snprintf(path, sizeof path, "d3d8_ring_test_%s.syms", mode);

    if (strcmp(mode, "cli") == 0) {
        if (!write_file(path, CLI_DUMP))
            return 2;
        setenv("RECOMP_XDK_SYMBOLS", path, 1);
        expect = DEVICE_GLOBAL;
    } else if (strcmp(mode, "tsv") == 0) {
        if (!write_file(path, TSV_DUMP))
            return 2;
        setenv("RECOMP_XDK_SYMBOLS", path, 1);
        expect = DEVICE_GLOBAL;
    } else if (strcmp(mode, "override") == 0) {
        setenv("RECOMP_D3D8_DEVICE_GLOBAL", "0x00ABCDEF", 1);
        expect = 0x00ABCDEFu;
    } else if (strcmp(mode, "missing") == 0) {
        /* A path that does not exist must not lose the title's own answer. */
        setenv("RECOMP_XDK_SYMBOLS", "/nonexistent/symbols.tsv", 1);
        given = DEVICE_GLOBAL;
        expect = DEVICE_GLOBAL;
    } else {
        unsetenv("RECOMP_XDK_SYMBOLS");
        unsetenv("RECOMP_D3D8_DEVICE_GLOBAL");
        given = DEVICE_GLOBAL;
        expect = DEVICE_GLOBAL;
    }

    d3d8_ring_set_defaults(given, 0x0019B200u);
    got = d3d8_ring_device_global();
    printf("%s[%s] resolved 0x%08X, expected 0x%08X (default was 0x%08X)\n",
           got == expect ? "PASS " : "FAIL ", mode, got, expect, given);
    return got == expect ? 0 : 1;
}
