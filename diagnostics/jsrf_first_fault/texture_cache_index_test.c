/* THE HASHED TEXTURE-CACHE INDEX CHANGES NOTHING BUT THE COST OF A LOOKUP.
 *
 * texture_buffer() in nv2a_metal.m found (source, size) with a linear scan
 * over every slot; RECOMP_METAL_TEXTURE_HASH replaces that with an
 * open-addressing index (25 Sep 2026). The claim is that the index is only an
 * index: the same request sequence must land in the same slots, with the same
 * hits, uploads, evictions, full/partial compares and changed counts, as the
 * scan. So this runs one seeded sequence through both and requires them
 * identical, request by request.
 *
 * The sequence is built to reach every path: a working set about three times
 * the slot count (so the cache fills and evicts continuously, which is what
 * exercises the index's deletion), a hot subset that hits, frame boundaries
 * (full compare) and repeats within a frame (partial probe), wholesale
 * rewrites mid-frame (partial-caught), one-byte edits (changed at the next
 * full compare), and one source address requested at two sizes (two keys).
 *
 * NEGATIVE CONTROL: mode 2 is the index with one deliberate bug -- a texture
 * that evicts another is not indexed. The test must see that as a mismatch,
 * or it is not able to see anything. */
#include "nv2a_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUESTS 60000u
static uint32_t rng;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

typedef struct { int *slot; unsigned long long c[8]; double ns_per_hit; } Run;
static const char *names[8] = { "requests", "hits", "uploads", "evictions", "full compares",
                                "partial compares", "partial-caught", "changed" };

static int run(int mode, unsigned slots, Run *out)
{
    const unsigned pool = slots * 3;
    uint8_t **tex = calloc(pool, sizeof *tex);
    size_t *size = calloc(pool, sizeof *size);
    out->slot = calloc(REQUESTS, sizeof *out->slot);
    if (!tex || !size || !out->slot) return 0;
    rng = 12345;
    for (unsigned i = 0; i < pool; ++i) {
        size[i] = 256u << (next() % 5);                  /* 256 .. 4096 bytes */
        tex[i] = malloc(4096);
        for (unsigned b = 0; b < 4096; ++b) tex[i][b] = (uint8_t)next();
    }
    nv2a_metal_texture_cache_test_reset(mode);
    for (unsigned r = 0; r < REQUESTS; ++r) {
        uint32_t x = next();
        unsigned i = (x % 4) ? x / 4 % (slots / 2)       /* hot: fits, hits */
                             : x / 4 % pool;             /* cold: evicts */
        size_t n = size[i];
        if (x % 97 == 0) n = n > 256 ? n / 2 : n * 2;    /* same source, second key */
        if (x % 211 == 0) memset(tex[i], (int)(x >> 3), 4096);   /* wholesale rewrite */
        else if (x % 101 == 0) tex[i][n / 2 + 1] ^= 0x5a;         /* between the probes */
        if (r % 400 == 0) nv2a_metal_texture_cache_test_frame();
        out->slot[r] = nv2a_metal_texture_cache_test_request(tex[i], n);
        if (out->slot[r] == -2) { fprintf(stderr, "no Metal device\n"); return 0; }
    }
    nv2a_metal_texture_cache_counters(out->c);

    /* Informational: the cost of a hit, all in one frame, hot set only. */
    {
        struct timespec a, b;
        unsigned hot = slots < 256 ? slots : 256, rounds = 200;
        nv2a_metal_texture_cache_test_frame();
        for (unsigned i = 0; i < hot; ++i) nv2a_metal_texture_cache_test_request(tex[i], size[i]);
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (unsigned k = 0; k < rounds; ++k)
            for (unsigned i = 0; i < hot; ++i) nv2a_metal_texture_cache_test_request(tex[i], size[i]);
        clock_gettime(CLOCK_MONOTONIC, &b);
        out->ns_per_hit = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / ((double)rounds * hot);
    }
    for (unsigned i = 0; i < pool; ++i) free(tex[i]);
    free(tex); free(size);
    return 1;
}

/* How many requests landed elsewhere, and how many counters differ. */
static unsigned differences(const char *what, const Run *a, const Run *b, int quiet)
{
    unsigned bad = 0, first = 0;
    for (unsigned r = 0; r < REQUESTS; ++r)
        if (a->slot[r] != b->slot[r]) { if (!bad) first = r; ++bad; }
    if (bad && !quiet)
        fprintf(stderr, "%s: %u of %u requests in a different slot; first #%u: scan %d, index %d\n",
                what, bad, REQUESTS, first, a->slot[first], b->slot[first]);
    for (unsigned k = 0; k < 8; ++k)
        if (a->c[k] != b->c[k]) {
            ++bad;
            if (!quiet) fprintf(stderr, "%s: %s scan %llu, index %llu\n", what, names[k], a->c[k], b->c[k]);
        }
    return bad;
}

int main(void)
{
    const char *e = getenv("RECOMP_METAL_TEXTURE_SLOTS");
    unsigned slots = (e && *e) ? (unsigned)strtoul(e, NULL, 10) : 512;
    if (slots < 16) slots = 16;
    if (slots > 1024) slots = 1024;
    Run scan, hash, broken;
    if (!run(0, slots, &scan) || !run(1, slots, &hash) || !run(2, slots, &broken)) return 1;

    fprintf(stderr, "%u slots, %u textures, %u requests\n", slots, slots * 3, REQUESTS);
    for (unsigned k = 0; k < 8; ++k)
        fprintf(stderr, "  %-17s scan %8llu  index %8llu  broken %8llu\n",
                names[k], scan.c[k], hash.c[k], broken.c[k]);
    fprintf(stderr, "  ns per hit        scan %8.1f  index %8.1f\n", scan.ns_per_hit, hash.ns_per_hit);

    /* The sequence must actually have reached every path, or equality proves
     * less than it says. */
    int reached = scan.c[1] && scan.c[3] > slots && scan.c[4] && scan.c[5] &&
                  scan.c[6] && scan.c[7] > scan.c[6];
    if (!reached) { fprintf(stderr, "FAIL: the sequence did not exercise every path\n"); return 1; }
    if (differences("index", &scan, &hash, 0)) { fprintf(stderr, "FAIL: the index changed behaviour\n"); return 1; }
    if (!differences("broken", &scan, &broken, 1)) {
        fprintf(stderr, "FAIL: the negative control (a broken index) went undetected\n");
        return 1;
    }
    fprintf(stderr, "ok   broken index detected (%u differences)\n", differences("broken", &scan, &broken, 1));
    puts("Texture cache: hashed index matches the linear scan slot for slot and counter for counter");
    return 0;
}
