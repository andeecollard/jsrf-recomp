/*
 * Resolving the D3D8 push-buffer device, and publishing an honest fence.
 * See d3d8_ring.h for why one address generalises the whole pump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "d3d8_ring.h"
#include "../recomp_switch.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static uint32_t s_default_global, s_default_device;
static uint32_t s_global;
static int s_resolved;

static uint32_t guest_u32(uint32_t va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    if (!mem || !va)
        return 0;
    return *(const volatile uint32_t *)(mem + va);
}

static void guest_store_u32(uint32_t va, uint32_t value)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    if (!mem || !va)
        return;
    *(volatile uint32_t *)(mem + va) = value;
}

void d3d8_ring_set_defaults(uint32_t global, uint32_t device_fallback)
{
    s_default_global = global;
    s_default_device = device_fallback;
}

/* One line of an XbSymbolDatabase CLI dump is `NAME = 0xADDRESS`; one line of
 * the merged TSV starts `0xADDRESS\t...\tNAME\t...`. Accept both, because the
 * merged table is what a session actually has to hand and re-running the scan
 * to satisfy a parser would be a silly reason to fail. Matched on the suffix
 * so the library prefix (`D3D8__`) is not load-bearing. */
static int name_matches(const char *field, const char *want, size_t nwant)
{
    size_t n = strlen(field);
    return n >= nwant && strcmp(field + n - nwant, want) == 0;
}

static uint32_t symbol_lookup(const char *path, const char *want)
{
    char line[512];
    size_t nwant = strlen(want);
    FILE *f = fopen(path, "r");
    uint32_t found = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');

        /* Trailing newline first, so the last field on a line is a name and
         * not a name plus whatever ended the line. */
        {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
        }
        if (eq) {
            /* XbSymbolDatabase CLI: `NAME = 0xADDRESS`. */
            char *name = line, *value = eq + 1;
            *eq = '\0';
            while (*name == ' ' || *name == '\t')
                name++;
            {
                size_t n = strlen(name);
                while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
                    name[--n] = '\0';
            }
            if (name_matches(name, want, nwant))
                found = (uint32_t)strtoul(value, NULL, 0);
            continue;
        }
        /* symbolize.py's TSV: `0xADDRESS \t ... \t NAME \t ...`. Every field
         * is offered to the matcher rather than trusting a column index, and
         * each is terminated at its own tab -- taking the rest of the line as
         * the name is what made this branch silently find nothing and fall
         * back to the built-in default, which a test read as a pass. */
        if (line[0] != '0')
            continue;
        {
            char *field = line, *tab;
            uint32_t va = (uint32_t)strtoul(line, NULL, 0);
            if (!va)
                continue;
            while ((tab = strchr(field, '\t')) != NULL) {
                *tab = '\0';
                if (name_matches(field, want, nwant)) {
                    found = va;
                    break;
                }
                field = tab + 1;
            }
            if (!found && name_matches(field, want, nwant))
                found = va;
        }
    }
    fclose(f);
    return found;
}

uint32_t d3d8_ring_device_global(void)
{
    if (!s_resolved) {
        const char *from = "the title's own constant";
        const char *env = getenv("RECOMP_D3D8_DEVICE_GLOBAL");

        s_resolved = 1;
        s_global = s_default_global;
        if (env && *env) {
            unsigned long va = strtoul(env, NULL, 0);
            if (va) {
                s_global = (uint32_t)va;
                from = "RECOMP_D3D8_DEVICE_GLOBAL";
            }
        } else {
            const char *syms = getenv("RECOMP_XDK_SYMBOLS");
            if (syms && *syms) {
                uint32_t va = symbol_lookup(syms, "D3D_g_pDevice");
                if (va) {
                    /* Say so when the scan disagrees with the title's own
                     * constant rather than silently preferring one: a
                     * disagreement means the constant is stale or the scan
                     * matched another title's build, and both are worth a
                     * line in the log. */
                    if (s_default_global && va != s_default_global)
                        fprintf(stderr, "  [D3D8-RING] symbol says D3D_g_pDevice"
                                " = 0x%08X, the built-in default says 0x%08X;"
                                " using the symbol\n", va, s_default_global);
                    s_global = va;
                    from = syms;
                }
            }
        }
        fprintf(stderr, "  [D3D8-RING] D3D_g_pDevice = 0x%08X (%s)%s\n",
                s_global, from,
                s_global ? "" : "  <<< no device global: the pump cannot run");
        fflush(stderr);
    }
    return s_global;
}

uint32_t d3d8_ring_device(void)
{
    uint32_t dev = guest_u32(d3d8_ring_device_global());
    /* Before the title creates its device the global is still zero, while the
     * struct itself may already be a fixed allocation. Falling back keeps the
     * early-boot window behaving exactly as the hardcoded constants did. */
    return dev ? dev : s_default_device;
}

uint32_t d3d8_ring_field_va(unsigned offset)
{
    uint32_t dev = d3d8_ring_device();
    return dev ? dev + offset : 0;
}

int d3d8_ring_publish_fence(uint32_t fence_word_va, int parser_drained,
                            uint32_t submitted)
{
    static unsigned long refused, published, reported;

    if (!fence_word_va)
        return 0;
    if (!parser_drained) {
        /* Not a knob. A pump that reaches here has told the title the ring is
         * free while its own parser is still reading, and the count is how a
         * run says so without anybody reading the pump's source. */
        if (++refused == 1 || (refused & 0xFFFFu) == 0) {
            fprintf(stderr, "  [D3D8-RING] refused to acknowledge with work"
                    " outstanding (%lu so far)\n", refused);
            fflush(stderr);
        }
        return 0;
    }
    if (guest_u32(fence_word_va) == submitted)
        return 0;
    guest_store_u32(fence_word_va, submitted);
    ++published;
    if ((published & 0xFFFFFu) == 0 && reported != published) {
        reported = published;
        fprintf(stderr, "  [D3D8-RING] fence published=%lu refused=%lu\n",
                published, refused);
        fflush(stderr);
    }
    return 1;
}
