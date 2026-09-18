#include "guest_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t va; char *name; } Entry;

static Entry *g_tab;
static size_t g_n;
static int    g_loaded;

static int by_va(const void *a, const void *b)
{
    uint32_t x = ((const Entry *)a)->va, y = ((const Entry *)b)->va;
    return x < y ? -1 : x > y ? 1 : 0;
}

int recomp_guest_va_from_symbol(const char *sym, uint32_t *out)
{
    const char *p;
    char *end;
    unsigned long v;

    if (!sym) return 0;
    p = sym;
    if (*p == '_') ++p;                 /* Mach-O leading underscore */
    if (strncmp(p, "sub_", 4) != 0) return 0;
    p += 4;
    if (!*p) return 0;
    v = strtoul(p, &end, 16);
    if (*end) return 0;                 /* trailing junk: not a plain sub_XXXX */
    if (out) *out = (uint32_t)v;
    return 1;
}

size_t recomp_guest_names_load(void)
{
    const char *path;
    FILE *f;
    char line[512];
    size_t cap = 0;

    if (g_loaded) return g_n;
    g_loaded = 1;                       /* a missing file is loaded-as-empty */

    path = getenv("RECOMP_GUEST_NAMES");
    if (!path || !*path) return 0;
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "  [GUEST-NAMES] cannot read %s -- crash reports will"
                        " print sub_ addresses only\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char *tab, *nl;
        unsigned long va;
        if (line[0] == '#' || line[0] == '\n') continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        va = strtoul(line, NULL, 16);
        if (!va) continue;
        nl = strchr(tab + 1, '\n');
        if (nl) *nl = '\0';
        if (!tab[1]) continue;
        if (g_n == cap) {
            size_t ncap = cap ? cap * 2 : 256;
            Entry *t = (Entry *)realloc(g_tab, ncap * sizeof *t);
            if (!t) break;
            g_tab = t; cap = ncap;
        }
        g_tab[g_n].va = (uint32_t)va;
        g_tab[g_n].name = strdup(tab + 1);
        if (!g_tab[g_n].name) break;
        ++g_n;
    }
    fclose(f);
    if (g_n) qsort(g_tab, g_n, sizeof *g_tab, by_va);
    fprintf(stderr, "  [GUEST-NAMES] %zu names from %s\n", g_n, path);
    return g_n;
}

const char *recomp_guest_name(uint32_t va)
{
    size_t lo = 0, hi = g_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_tab[mid].va < va) lo = mid + 1;
        else if (g_tab[mid].va > va) hi = mid;
        else return g_tab[mid].name;
    }
    return NULL;
}

size_t recomp_guest_names_count(void) { return g_n; }
