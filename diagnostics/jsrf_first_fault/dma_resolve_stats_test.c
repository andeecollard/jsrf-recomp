/* Does the RAMHT scan counter count what its name says?
 *
 * nv2a_dma_resolve's new scans/entries/misses/worst counters exist to decide
 * one thing: whether memoising the RAMHT lookup is worth writing. That makes
 * them exactly the kind of counter this project has been burned by -- a number
 * that gets quoted in a decision without anyone having read what increments
 * it. So read it here.
 *
 * The four properties that make the number mean what the [DMA] report line
 * claims:
 *
 *   1. entries counts ENTRIES EXAMINED, not calls. A hit at slot N costs N+1.
 *   2. a full miss walks the WHOLE table, and is counted as the whole table.
 *      This is the case a cache cannot help -- there is nothing to cache --
 *      so it has to be separable from the hits.
 *   3. a call rejected on its ARGUMENTS examined nothing and is not a scan.
 *      Counting it would pull entries-per-scan towards zero and make the table
 *      look cheaper to walk than it is.
 *   4. the table size comes from RAMHT bits 17:16, so a miss on a 32K table
 *      must cost eight times a miss on a 4K one.
 *
 * Every assertion below is paired: an absence is only checked next to a
 * presence that moved, so "the counter did not increment" can never be
 * confused with "the counter is dead".
 *
 * Run with --bench to print the timings the [DMA] line's 0.53 ns/entry
 * constant comes from, and the clock cost pb_walk_on quotes. Those are timings
 * on whatever host you are on, so they are not assertions -- a loaded machine
 * would fail them for no reason.
 */
#include "nv2a_texture_copy.h"
#include "texture_copy_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

extern void nv2a_dma_resolve_stats(unsigned long long *scans,
                                   unsigned long long *entries,
                                   unsigned long long *misses,
                                   unsigned long long *worst);

static uint8_t ramin[0x100000];

/* The DMA instance record every hash entry below points at: a valid
 * NV_DMA_IN_MEMORY context with base 0x2345 and limit 0xFFFF. Taken from
 * copy_dma() in texture_copy_state.h, which is the shape measured off JSRF. */
static void put_instance(void)
{
    le32(ramin + 0x1120, 0xb03d | (0x2345 & 4095) << 20);
    le32(ramin + 0x1124, 0xffff);
    le32(ramin + 0x1128, (0x2345 & ~4095u) | 3);
}

/* A valid hash entry for `handle` at entry index `idx`. Index, not byte
 * offset: the whole question here is how many eight-byte entries get walked. */
static void put_entry(size_t idx, uint32_t handle)
{
    le32(ramin + idx * 8, handle);
    le32(ramin + idx * 8 + 4, 0x80000112);   /* instance 0x1120, channel 0 */
}

static void reset_table(unsigned shift)
{
    memset(ramin, 0, 4096u << shift);
    put_instance();
}

/* RAMHT register value: base 0 (bits 8:4), size shift in bits 17:16. */
static uint32_t ramht_for(unsigned shift) { return shift << 16; }

struct stats { unsigned long long scans, entries, misses, worst; };
static struct stats read_stats(void)
{
    struct stats s;
    nv2a_dma_resolve_stats(&s.scans, &s.entries, &s.misses, &s.worst);
    return s;
}

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void bench(void)
{
    const unsigned REPS = 200000;
    uint32_t base, limit;
    volatile uint32_t sink = 0;
    unsigned shift, i;

    {
        double t0 = now_ns();
        for (i = 0; i < REPS; ++i) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            sink += (uint32_t)ts.tv_nsec;
        }
        printf("clock_gettime(CLOCK_MONOTONIC): %.1f ns/call"
               "  (pb_now_us pays this twice per timed region)\n",
               (now_ns() - t0) / REPS);
    }

    for (shift = 0; shift < 4; ++shift) {
        uint32_t ramht = ramht_for(shift);
        size_t entries = (4096u << shift) / 8u;
        size_t at[5];
        unsigned p;
        at[0] = 3; at[1] = entries / 16; at[2] = entries / 4;
        at[3] = entries / 2; at[4] = entries - 1;
        printf("\nRAMHT %u bytes, %zu entries\n", 4096u << shift, entries);
        for (p = 0; p < 5; ++p) {
            double t0, per;
            reset_table(shift);
            put_entry(at[p], 3);
            CHECK(nv2a_dma_resolve(ramin, sizeof ramin, ramht, 3, &base, &limit));
            t0 = now_ns();
            for (i = 0; i < REPS; ++i) {
                nv2a_dma_resolve(ramin, sizeof ramin, ramht, 3, &base, &limit);
                sink += base;
            }
            per = (now_ns() - t0) / REPS;
            printf("  hit at entry %5zu: %8.1f ns  (%.3f ns/entry)\n",
                   at[p], per, per / (double)(at[p] + 1));
        }
        {
            double t0 = now_ns(), per;
            for (i = 0; i < REPS; ++i)
                sink += (unsigned)nv2a_dma_resolve(ramin, sizeof ramin, ramht,
                                                   0x777, &base, &limit);
            per = (now_ns() - t0) / REPS;
            printf("  MISS (full scan) : %8.1f ns  (%.3f ns/entry)\n",
                   per, per / (double)entries);
        }
    }

    /* The other half of prepare_texture_copy, for scale: if this is small and
     * the scans are small, the whole `prepare` stage is small. */
    {
        static uint32_t methods[2048];
        NV2ATextureCopy s;
        double t0;
        copy_methods(methods, 256, 256, 1024, 1280, 4);
        CHECK(!nv2a_texture_copy_prepare(methods, &s));
        t0 = now_ns();
        for (i = 0; i < REPS; ++i) {
            nv2a_texture_copy_prepare(methods, &s);
            sink += s.width;
        }
        printf("\nnv2a_texture_copy_prepare: %.1f ns/call"
               " (memset of %zu bytes plus the validation loops)\n",
               (now_ns() - t0) / REPS, sizeof(NV2ATextureCopy));
    }
    (void)sink;
}

int main(int argc, char **argv)
{
    uint32_t base, limit;
    struct stats a, b;

    /* A fresh process has done no scans. If this is not zero the counters are
     * being moved by something other than the calls below and nothing after it
     * means anything. */
    a = read_stats();
    CHECK(a.scans == 0 && a.entries == 0 && a.misses == 0 && a.worst == 0);

    /* (1) A hit at entry 3 examines four entries. */
    reset_table(0);
    put_entry(3, 3);
    CHECK(nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(0), 3, &base, &limit));
    CHECK(base == 0x2345 && limit == 0xffff);   /* the resolve itself still works */
    b = read_stats();
    CHECK(b.scans == 1);
    CHECK(b.entries == 4);
    CHECK(b.misses == 0);
    CHECK(b.worst == 4);

    /* A hit further in costs proportionally more -- this is the property the
     * whole memoisation question turns on, so assert the slope, not just that
     * the number moved. */
    a = b;
    reset_table(0);
    put_entry(200, 3);
    CHECK(nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(0), 3, &base, &limit));
    b = read_stats();
    CHECK(b.scans == a.scans + 1);
    CHECK(b.entries == a.entries + 201);
    CHECK(b.worst == 201);                      /* worst tracks the maximum */

    /* (2) A full miss walks the whole 4096-byte table: 512 entries, and it is
     * counted as a miss, separately from the hits above. */
    a = b;
    CHECK(!nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(0), 0x777, &base, &limit));
    b = read_stats();
    CHECK(b.scans == a.scans + 1);
    CHECK(b.entries == a.entries + 512);
    CHECK(b.misses == a.misses + 1);
    CHECK(b.worst == 512);

    /* (3) Calls rejected on their arguments examined nothing, so they are not
     * scans. Paired with a real scan immediately afterwards so that "scans did
     * not move" is distinguishable from "the counter stopped working". */
    a = b;
    CHECK(!nv2a_dma_resolve(NULL, sizeof ramin, ramht_for(0), 3, &base, &limit));
    CHECK(!nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(0), 0, &base, &limit));
    CHECK(!nv2a_dma_resolve(ramin, 2048, ramht_for(0), 3, &base, &limit));
    b = read_stats();
    CHECK(b.scans == a.scans);                  /* the absence ... */
    CHECK(b.entries == a.entries);
    put_entry(3, 3);
    CHECK(nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(0), 3, &base, &limit));
    b = read_stats();
    CHECK(b.scans == a.scans + 1);              /* ... beside a positive control */
    CHECK(b.entries == a.entries + 4);

    /* (4) The table size comes from RAMHT bits 17:16, so the miss cost scales
     * with it. 4096 entries is the largest a scan can be, and it is the number
     * the "is a cache worth it" arithmetic in nv2a_pb_exec.c is bounded by. */
    a = b;
    reset_table(3);
    CHECK(!nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(3), 0x777, &base, &limit));
    b = read_stats();
    CHECK(b.entries == a.entries + 4096);
    CHECK(b.misses == a.misses + 1);
    CHECK(b.worst == 4096);

    /* A hit found at a given slot costs the same whatever the table size: the
     * scan stops at the entry, it does not walk to the end. Without this, a
     * large `worst` could be read as "every scan is expensive" when it only
     * means one miss happened. */
    a = b;
    reset_table(3);
    put_entry(7, 9);
    CHECK(nv2a_dma_resolve(ramin, sizeof ramin, ramht_for(3), 9, &base, &limit));
    b = read_stats();
    CHECK(b.entries == a.entries + 8);
    CHECK(b.worst == 4096);                     /* unchanged: 8 is not a new max */

    if (argc > 1 && !strcmp(argv[1], "--bench"))
        bench();

    printf("dma_resolve_stats: ok\n");
    return 0;
}
