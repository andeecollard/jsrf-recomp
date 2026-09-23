/* Filtered tracing for stores emitted by the static recompiler.
 *
 * This intentionally does not intercept kernel/HLE or device writes.  Those
 * sources do not have a translated guest instruction PC and need separate
 * provenance when they are added. */
#include "recomp_mem_watch.h"
#include "d3d8_ring.h"

#include <errno.h>
#include <stdatomic.h>
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
static int s_watch_raw_va;
static int s_tally_ring;          /* RECOMP_MEM_WATCH_TALLY=ring, see below */

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
    uint64_t range_end;

    g_recomp_mem_watch_enabled = s_tally_ring;
    s_watch_length = 0;
    if (!spec || !*spec)
        return 0;
    if (!parse_u32(spec, &end, &va) || *end != ':' ||
        !parse_u32(end + 1, &end, &length) || *end != '\0' || !length) {
        fprintf(stderr,
                "[MEM-WATCH] invalid RECOMP_MEM_WATCH='%s'"
                " (expected <guest_va>:<length>)\n", spec);
        return 0;
    }
    if (!checked_end(va, length, &range_end)) {
        fprintf(stderr, "[MEM-WATCH] invalid range va=0x%08X length=0x%X\n",
                va, length);
        return 0;
    }

    /* RAM has several aliases, so normalize it to byte identity. MMIO has no
     * such aliases: compare an aperture watch by literal guest VA. The store
     * helper already has the instruction's valid host pointer, so accepting a
     * raw range here neither maps nor touches any additional memory. */
    s_watch_raw_va = !normalize_ram(va, length, &normalized);
    if (s_watch_raw_va) normalized = va;

    s_watch_ram_lo = normalized;
    s_watch_length = length;
    g_recomp_mem_watch_enabled = 1;
    fprintf(stderr,
            "[MEM-WATCH] armed source=guest va=0x%08X length=0x%X"
            " %s=0x%08X aliases=%s\n",
            va, length, s_watch_raw_va ? "raw" : "ram", normalized,
            s_watch_raw_va ? "off" : "on");
    fflush(stderr);
    return 1;
}


/* ── Ring-store tally: RECOMP_MEM_WATCH_TALLY=ring (G33, 23 Sep 2026) ───────
 *
 * The D3D8-lift plan needs one fact no static scan can give: does anything
 * outside the D3D library write the GPU command ring, including through a
 * pointer the game computed? This mode answers it by counting, per guest
 * function, every translated store and block write that lands in the live
 * ring (device +0x24 .. +0x28, read through d3d8_ring's self-checked
 * accessor and compared as RAM identity, so any mirror of the ring counts).
 *
 * It prints nothing per store. A table goes to stderr every 2^22 ring stores
 * and at exit; classify the functions afterwards against the D3D section.
 * Stores made by host code (kernel bridges, devices) have no guest PC and are
 * not seen, which is the right scope: the question is about guest code. */
#define TALLY_SLOTS 4096u
static _Atomic uint32_t s_tally_fn[TALLY_SLOTS];
static _Atomic uint64_t s_tally_stores[TALLY_SLOTS];
static _Atomic uint64_t s_tally_block_bytes[TALLY_SLOTS];
static _Atomic uint64_t s_tally_total, s_tally_dropped, s_tally_seen;
static _Atomic uint32_t s_ring_lo_ram, s_ring_hi_ram;   /* hi == 0: unknown */
static _Atomic uint32_t s_ring_lo_raw, s_ring_hi_raw;   /* the VAs as the device holds them */

static void tally_refresh_ring(void)
{
    D3D8RingTarget t;
    uint32_t lo;

    if (!d3d8_ring_read_target(&t) || !t.trusted)
        return;
    /* Raw bounds always; RAM identity too when the ring lies in a window the
     * alias model knows, so a store through another mirror still counts. */
    atomic_store_explicit(&s_ring_lo_raw, t.ring_lo, memory_order_relaxed);
    atomic_store_explicit(&s_ring_hi_raw, t.ring_hi, memory_order_relaxed);
    if (!normalize_ram(t.ring_lo, (size_t)(t.ring_hi - t.ring_lo), &lo))
        return;
    atomic_store_explicit(&s_ring_lo_ram, lo, memory_order_relaxed);
    atomic_store_explicit(&s_ring_hi_ram, lo + (t.ring_hi - t.ring_lo),
                          memory_order_relaxed);
}

static void tally_add(uint32_t function, uint64_t stores, uint64_t bytes)
{
    uint32_t key = function ? function : 1u;   /* 0 marks an empty slot */
    uint32_t slot = (key * 2654435761u) >> 20;

    for (unsigned probe = 0; probe < TALLY_SLOTS; ++probe) {
        uint32_t i = (slot + probe) & (TALLY_SLOTS - 1u);
        uint32_t cur = atomic_load_explicit(&s_tally_fn[i], memory_order_relaxed);
        if (cur == 0) {
            uint32_t expect = 0;
            if (!atomic_compare_exchange_strong(&s_tally_fn[i], &expect, key))
                cur = expect;
            else
                cur = key;
        }
        if (cur == key) {
            atomic_fetch_add_explicit(&s_tally_stores[i], stores, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_tally_block_bytes[i], bytes, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&s_tally_dropped, 1, memory_order_relaxed);
}

void recomp_mem_watch_tally_report(const char *why)
{
    uint32_t lo, hi;
    unsigned used = 0;

    if (!s_tally_ring)
        return;
    lo = atomic_load_explicit(&s_ring_lo_ram, memory_order_relaxed);
    hi = atomic_load_explicit(&s_ring_hi_ram, memory_order_relaxed);
    fprintf(stderr,
            "[RING-TALLY] %s ring=0x%08X..0x%08X ring_ram=0x%08X..0x%08X"
            " total=%llu dropped=%llu\n",
            why, atomic_load(&s_ring_lo_raw), atomic_load(&s_ring_hi_raw), lo, hi,
            (unsigned long long)atomic_load(&s_tally_total),
            (unsigned long long)atomic_load(&s_tally_dropped));
    for (uint32_t i = 0; i < TALLY_SLOTS; ++i) {
        uint32_t fn = atomic_load_explicit(&s_tally_fn[i], memory_order_relaxed);
        if (!fn)
            continue;
        ++used;
        fprintf(stderr, "[RING-TALLY] function=0x%08X stores=%llu block_bytes=%llu\n",
                fn,
                (unsigned long long)atomic_load(&s_tally_stores[i]),
                (unsigned long long)atomic_load(&s_tally_block_bytes[i]));
    }
    fprintf(stderr, "[RING-TALLY] %s functions=%u\n", why, used);
    fflush(stderr);
}

static void tally_report_at_exit(void) { recomp_mem_watch_tally_report("exit"); }

static void tally_reset(void)
{
    for (uint32_t i = 0; i < TALLY_SLOTS; ++i) {
        atomic_store(&s_tally_fn[i], 0);
        atomic_store(&s_tally_stores[i], 0);
        atomic_store(&s_tally_block_bytes[i], 0);
    }
    atomic_store(&s_tally_total, 0);
    atomic_store(&s_tally_dropped, 0);
    atomic_store(&s_tally_seen, 0);
    atomic_store(&s_ring_lo_ram, 0);
    atomic_store(&s_ring_hi_ram, 0);
    atomic_store(&s_ring_lo_raw, 0);
    atomic_store(&s_ring_hi_raw, 0);
}

/* Does the guest range [va, va+len) touch the ring, by raw VA or by RAM
 * identity? Bounds refresh lazily: every 4096 checks until known, then every
 * 2^20, which also follows a device Reset. */
static int tally_hits_ring(uint32_t va, uint64_t len)
{
    uint64_t seen = atomic_fetch_add_explicit(&s_tally_seen, 1, memory_order_relaxed);
    uint32_t hi_raw = atomic_load_explicit(&s_ring_hi_raw, memory_order_relaxed);
    uint32_t lo, hi, ram;

    if ((!hi_raw && (seen & 4095u) == 0) || (seen & ((1u << 20) - 1u)) == 0) {
        tally_refresh_ring();
        hi_raw = atomic_load_explicit(&s_ring_hi_raw, memory_order_relaxed);
    }
    if (!hi_raw)
        return 0;
    lo = atomic_load_explicit(&s_ring_lo_raw, memory_order_relaxed);
    if ((uint64_t)va < hi_raw && (uint64_t)lo < (uint64_t)va + len)
        return 1;
    hi = atomic_load_explicit(&s_ring_hi_ram, memory_order_relaxed);
    if (!hi || len > UINT32_MAX || !normalize_ram(va, (size_t)len, &ram))
        return 0;
    lo = atomic_load_explicit(&s_ring_lo_ram, memory_order_relaxed);
    return (uint64_t)ram < hi && (uint64_t)lo < (uint64_t)ram + len;
}

static void tally_store(uint32_t function, uint32_t va, unsigned width)
{
    if (!tally_hits_ring(va, width))
        return;
    tally_add(function, 1, 0);
    if ((atomic_fetch_add_explicit(&s_tally_total, 1, memory_order_relaxed)
         & ((1u << 22) - 1u)) == ((1u << 22) - 1u))
        recomp_mem_watch_tally_report("periodic");
}

static void tally_block(uint32_t function, uint32_t dst, uint32_t len)
{
    uint32_t lo = len < dst ? dst - len : 0;

    /* Same conservative two-direction span as the range watch below. */
    if (tally_hits_ring(lo, (uint64_t)(dst - lo) + len))
        tally_add(function, 0, len);
}

static void tally_configure(void)
{
    const char *mode = getenv("RECOMP_MEM_WATCH_TALLY");

    if (!mode || strcmp(mode, "ring") != 0 || s_tally_ring)
        return;
    static int exit_hook;
    tally_reset();
    s_tally_ring = 1;
    g_recomp_mem_watch_enabled = 1;
    if (!exit_hook) {
        exit_hook = 1;
        atexit(tally_report_at_exit);
    }
    fprintf(stderr, "[RING-TALLY] armed: counting guest stores into the D3D ring by function\n");
    fflush(stderr);
}

void recomp_mem_watch_init(size_t ram_span, uint32_t mirror_mask,
                           uint32_t tiled_base, size_t tiled_span)
{
    s_ram_span = ram_span;
    s_mirror_mask = mirror_mask;
    s_tiled_base = tiled_base;
    s_tiled_span = tiled_span;
    s_heap_alias_span = 0;
    tally_configure();
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
    if (!s_watch_length && getenv("RECOMP_MEM_WATCH"))
        configure(getenv("RECOMP_MEM_WATCH"));
}

/* The inverse of normalize_ram.  A resident render target lives at one RAM
 * offset and is readable at every window that aliases it; the ownership map
 * has to be armed at all of them, because nothing constrains which one the
 * guest's own pointer arithmetic produced. */
unsigned recomp_mem_watch_ram_aliases(uint32_t ram_offset, size_t span,
                                      uint32_t *out_va, unsigned max)
{
    uint64_t end;
    unsigned n = 0;

    if (!out_va || !max || !checked_end(ram_offset, span, &end))
        return 0;
    if (!s_ram_span || end > s_ram_span) {
        /* Not RAM as this module understands it -- an MMIO aperture, or a
         * range that runs off the end of the mapping.  The literal address is
         * still worth arming; it just has no aliases to add. */
        out_va[n++] = ram_offset;
        return n;
    }

    out_va[n++] = ram_offset;
    for (unsigned i = 0; i < RECOMP_MEM_WATCH_MIRRORS && n < max; ++i) {
        uint64_t base;
        if (!(s_mirror_mask & (1u << i)))
            continue;
        base = (uint64_t)(i + 1u) * s_ram_span + ram_offset;
        if (base + span <= 0x100000000ULL)
            out_va[n++] = (uint32_t)base;
    }
    if (n < max && s_tiled_span && end <= s_tiled_span)
        out_va[n++] = s_tiled_base + ram_offset;
    if (n < max && s_heap_alias_span
            && (uint64_t)ram_offset >= s_heap_alias_ram_offset
            && end <= (uint64_t)s_heap_alias_ram_offset + s_heap_alias_span)
        out_va[n++] = s_heap_alias_base
                    + (ram_offset - s_heap_alias_ram_offset);
    return n;
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
    s_watch_raw_va = 0;
    s_tally_ring = 0;
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

    if (s_tally_ring) {
        tally_store(guest_function, guest_va, width);
        if (!s_watch_length) {
            if (host_ptr)
                store_width(host_ptr, width, new_value);
            return;
        }
    }

    if (host_ptr && (width == 1 || width == 2 || width == 4 || width == 8) &&
        (s_watch_raw_va || normalize_ram(guest_va, width, &ram))) {
        if (s_watch_raw_va) ram = guest_va;
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
    if (s_tally_ring) {
        tally_block(guest_function, dst_va, len);
        if (!s_watch_length)
            return;
    }

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
