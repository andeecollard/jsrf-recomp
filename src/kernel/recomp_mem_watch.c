/* Filtered tracing for stores emitted by the static recompiler.
 *
 * This intentionally does not intercept kernel/HLE or device writes.  Those
 * sources do not have a translated guest instruction PC and need separate
 * provenance when they are added. */
#include "recomp_mem_watch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_MEM_WATCH_MIRRORS 28u

int g_recomp_mem_watch_enabled;

static size_t s_ram_span;
static uint32_t s_mirror_mask;
static uint32_t s_tiled_base;
static size_t s_tiled_span;
static uint32_t s_heap_alias_base;
static uint32_t s_heap_alias_ram_offset;
static size_t s_heap_alias_span;
static uint32_t s_watch_ram_lo;
static uint32_t s_watch_length;

static int checked_end(uint32_t start, size_t bytes, uint64_t *end)
{
    uint64_t e = (uint64_t)start + bytes;
    if (!bytes || e > 0x100000000ULL)
        return 0;
    *end = e;
    return 1;
}

/* Resolve a fully contained access to the byte identity in the shared RAM
 * mapping. Only views known to share that mapping participate; skipped mirror
 * slots and the ordinary contiguous aperture remain distinct. */
static int normalize_ram(uint32_t va, size_t bytes, uint32_t *ram_offset)
{
    uint64_t end;

    if (!s_ram_span || s_ram_span > UINT32_MAX ||
        !checked_end(va, bytes, &end))
        return 0;

    if ((uint64_t)va < s_ram_span && end <= s_ram_span) {
        *ram_offset = va;
        return 1;
    }

    for (unsigned i = 0; i < RECOMP_MEM_WATCH_MIRRORS; ++i) {
        uint64_t lo, hi;
        if (!(s_mirror_mask & (1u << i)))
            continue;
        lo = (uint64_t)(i + 1u) * s_ram_span;
        hi = lo + s_ram_span;
        if ((uint64_t)va >= lo && end <= hi) {
            *ram_offset = (uint32_t)((uint64_t)va - lo);
            return 1;
        }
    }

    if (s_tiled_span) {
        uint64_t lo = s_tiled_base;
        uint64_t hi = lo + s_tiled_span;
        if ((uint64_t)va >= lo && end <= hi) {
            *ram_offset = va - s_tiled_base;
            return 1;
        }
    }

    /* The optional CPU physical view replaces a slice of the otherwise
     * separate contiguous aperture with pages from the low RAM mapping. */
    if (s_heap_alias_span) {
        uint64_t lo = s_heap_alias_base;
        uint64_t hi = lo + s_heap_alias_span;
        if ((uint64_t)va >= lo && end <= hi) {
            *ram_offset = s_heap_alias_ram_offset + (va - s_heap_alias_base);
            return 1;
        }
    }

    return 0;
}

static int parse_u32(const char *text, const char **end_out, uint32_t *value)
{
    char *end;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno || end == text || parsed > UINT32_MAX)
        return 0;
    *end_out = end;
    *value = (uint32_t)parsed;
    return 1;
}

static int configure(const char *spec)
{
    const char *end;
    uint32_t va, length, normalized;

    g_recomp_mem_watch_enabled = 0;
    if (!spec || !*spec)
        return 0;
    if (!parse_u32(spec, &end, &va) || *end != ':' ||
        !parse_u32(end + 1, &end, &length) || *end != '\0' || !length) {
        fprintf(stderr,
                "[MEM-WATCH] invalid RECOMP_MEM_WATCH='%s'"
                " (expected <guest_va>:<length>)\n", spec);
        return 0;
    }
    if (!normalize_ram(va, length, &normalized)) {
        fprintf(stderr,
                "[MEM-WATCH] unsupported range va=0x%08X length=0x%X;"
                " v1 accepts one fully mapped RAM/alias range\n",
                va, length);
        return 0;
    }

    s_watch_ram_lo = normalized;
    s_watch_length = length;
    g_recomp_mem_watch_enabled = 1;
    fprintf(stderr,
            "[MEM-WATCH] armed source=guest va=0x%08X length=0x%X"
            " ram=0x%08X aliases=on\n",
            va, length, normalized);
    fflush(stderr);
    return 1;
}

void recomp_mem_watch_init(size_t ram_span, uint32_t mirror_mask,
                           uint32_t tiled_base, size_t tiled_span)
{
    s_ram_span = ram_span;
    s_mirror_mask = mirror_mask;
    s_tiled_base = tiled_base;
    s_tiled_span = tiled_span;
    s_heap_alias_span = 0;
    configure(getenv("RECOMP_MEM_WATCH"));
}

void recomp_mem_watch_add_ram_alias(uint32_t guest_base,
                                    uint32_t ram_offset, size_t span)
{
    s_heap_alias_base = guest_base;
    s_heap_alias_ram_offset = ram_offset;
    s_heap_alias_span = span;

    /* A watch expressed through this dynamically enabled alias could not be
     * normalized during layout init. Retry it now that the view is real. */
    if (!g_recomp_mem_watch_enabled && getenv("RECOMP_MEM_WATCH"))
        configure(getenv("RECOMP_MEM_WATCH"));
}

void recomp_mem_watch_shutdown(void)
{
    g_recomp_mem_watch_enabled = 0;
    s_ram_span = 0;
    s_mirror_mask = 0;
    s_tiled_span = 0;
    s_heap_alias_span = 0;
    s_watch_ram_lo = 0;
    s_watch_length = 0;
}

static uint64_t load_width(volatile void *ptr, unsigned width)
{
    switch (width) {
    case 1: return *(volatile uint8_t *)ptr;
    case 2: return *(volatile uint16_t *)ptr;
    case 4: return *(volatile uint32_t *)ptr;
    case 8: return *(volatile uint64_t *)ptr;
    default: return 0;
    }
}

static void store_width(volatile void *ptr, unsigned width, uint64_t value)
{
    switch (width) {
    case 1: *(volatile uint8_t *)ptr = (uint8_t)value; break;
    case 2: *(volatile uint16_t *)ptr = (uint16_t)value; break;
    case 4: *(volatile uint32_t *)ptr = (uint32_t)value; break;
    case 8: *(volatile uint64_t *)ptr = value; break;
    default: break;
    }
}

void recomp_mem_watch_guest_store(uint32_t guest_pc, uint32_t guest_function,
                                  uint32_t guest_va, unsigned width,
                                  volatile void *host_ptr, uint64_t new_value)
{
    uint32_t ram;
    uint64_t access_hi, watch_hi;
    int match = 0;
    uint64_t old_value = 0;

    if (host_ptr && (width == 1 || width == 2 || width == 4 || width == 8) &&
        normalize_ram(guest_va, width, &ram)) {
        access_hi = (uint64_t)ram + width;
        watch_hi = (uint64_t)s_watch_ram_lo + s_watch_length;
        match = (uint64_t)ram < watch_hi &&
                (uint64_t)s_watch_ram_lo < access_hi;
    }

    if (match)
        old_value = load_width(host_ptr, width);
    if (host_ptr)
        store_width(host_ptr, width, new_value);

    if (match) {
        unsigned digits = width * 2u;
        fprintf(stderr,
                "[MEM-WATCH] source=guest pc=0x%08X function=0x%08X va=0x%08X"
                " ram=0x%08X width=%u old=0x%0*llX new=0x%0*llX\n",
                guest_pc, guest_function, guest_va, ram, width,
                (int)digits, (unsigned long long)old_value,
                (int)digits, (unsigned long long)new_value);
        fflush(stderr);
    }
}

/* Block writes -- the hole this watch had from the start.
 *
 * recomp_mem_watch_guest_store only sees stores routed through the
 * RECOMP_MEM_WRITE* macros. `rep movs` and `rep stos` are lifted to memcpy and
 * to raw MEM32/byte loops that bypass those macros entirely, so every block
 * copy and fill in the title was invisible here. That is not a corner case: it
 * is how a title clears a structure, and it is exactly what was overwriting
 * the XPP list head at 0x002648D4 while this watch reported nothing.
 *
 * Called BEFORE the copy runs, with the destination cursor and the byte count.
 * The range is checked in BOTH directions because a string op with the
 * direction flag set walks downward from the cursor; the watch window is a
 * handful of bytes, so being generous here costs nothing and missing a
 * backward copy would cost another session.
 *
 * No old/new values: the point is to name the writer. The surrounding
 * per-store lines already give the values. */
void recomp_mem_watch_guest_block(uint32_t guest_function, uint32_t dst_va,
                                  uint32_t len)
{
    uint64_t lo, hi, watch_hi;
    uint32_t ram;

    if (!g_recomp_mem_watch_enabled || !len)
        return;

    /* Conservative span: [dst-len, dst+len). */
    lo = (uint64_t)dst_va > (uint64_t)len ? (uint64_t)dst_va - len : 0;
    hi = (uint64_t)dst_va + len;

    /* normalize_ram needs a fully mapped range; fall back to the raw VA when
     * the span crosses something it will not resolve, rather than going quiet.
     * A false positive here is a line of log; a false negative is the bug. */
    if (!normalize_ram((uint32_t)lo, (size_t)(hi - lo), &ram))
        ram = (uint32_t)lo;

    watch_hi = (uint64_t)s_watch_ram_lo + s_watch_length;
    if ((uint64_t)ram >= watch_hi || (uint64_t)s_watch_ram_lo >= (uint64_t)ram + (hi - lo))
        return;

    fprintf(stderr,
            "[MEM-WATCH] source=guest-block function=0x%08X dst=0x%08X"
            " len=%u (span 0x%08llX..0x%08llX)\n",
            guest_function, dst_va, len,
            (unsigned long long)lo, (unsigned long long)hi);
    fflush(stderr);
}
