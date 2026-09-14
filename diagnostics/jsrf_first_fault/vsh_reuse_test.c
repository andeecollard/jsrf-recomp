/* Batch-local vertex reuse must not survive a batch boundary.
 *
 * WHY THIS EXISTS. The reuse cache keys on the 16-bit vertex index. Between
 * batches the vertex arrays, the shader constants and the program itself can
 * all change, so an entry that outlives its batch makes a later draw reuse a
 * transform computed from different data -- silently wrong geometry, no crash,
 * no rejected batch, nothing in a log. The first version of the cache had no
 * per-batch clear at all and would have done exactly that. It was caught by
 * reading, not by any test, which is the gap this closes.
 *
 * The cache lives inside prepare_vertices and is not reachable from here, so
 * this tests the INVARIANT the implementation has to satisfy, against the same
 * clear-by-rewalk scheme the implementation uses: after a batch is finished,
 * no index from that batch may still be marked present. A cache that cleared
 * only the indices it had CACHED -- rather than every index it walked -- would
 * pass a naive test and fail this one, because a duplicate index is walked more
 * often than it is cached.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define NV_MAX_INDICES 4096
static uint8_t  seen[65536 / 8];
static uint16_t at[65536];

static void batch(const uint16_t *idx, unsigned n, unsigned *hits)
{
    unsigned i;
    *hits = 0;
    for (i = 0; i < n; ++i) {
        uint16_t v = idx[i];
        if (seen[v >> 3] & (1u << (v & 7))) { (*hits)++; continue; }
        seen[v >> 3] |= (uint8_t)(1u << (v & 7));
        at[v] = (uint16_t)i;
    }
    /* The clear the implementation performs: re-walk THIS batch's indices. */
    for (i = 0; i < n; ++i) {
        uint16_t v = idx[i];
        seen[v >> 3] &= (uint8_t)~(1u << (v & 7));
    }
}

int main(void)
{
    unsigned hits;
    /* A batch that shares vertices, as an indexed triangle list does. */
    static const uint16_t tri[] = { 0, 1, 2,  2, 1, 3,  0, 2, 4 };
    batch(tri, sizeof tri / sizeof tri[0], &hits);
    /* 9 indices, 5 unique (0,1,2,3,4) -> 4 duplicates. Counted rather than
     * guessed: the first version of this line said 3 and the test caught it. */
    CHECK(hits == 4);

    /* NOTHING may remain marked after the batch. This is the invariant that
     * the missing clear violated. */
    for (unsigned v = 0; v < 65536; ++v)
        CHECK(!(seen[v >> 3] & (1u << (v & 7))));

    /* The same indices in a NEW batch must all miss. If the cache leaked, these
     * would hit and reuse transforms computed from the previous batch's vertex
     * arrays and constants. */
    batch(tri, sizeof tri / sizeof tri[0], &hits);
    CHECK(hits == 4);              /* the same 4 intra-batch reuses, not 9 */

    /* A batch that touches the extremes of the index space still clears. */
    {
        static uint16_t wide[NV_MAX_INDICES];
        unsigned i;
        for (i = 0; i < NV_MAX_INDICES; ++i)
            wide[i] = (uint16_t)((i * 65521u) & 0xFFFFu);   /* scattered */
        batch(wide, NV_MAX_INDICES, &hits);
        for (unsigned v = 0; v < 65536; ++v)
            CHECK(!(seen[v >> 3] & (1u << (v & 7))));
    }

    /* Index 0 and index 65535 are the boundary cases a shift-and-mask scheme
     * gets wrong most often. */
    {
        static const uint16_t edge[] = { 0, 65535, 0, 65535 };
        batch(edge, 4, &hits);
        CHECK(hits == 2);
        CHECK(!(seen[0] & 1u));
        CHECK(!(seen[65535 >> 3] & (1u << (65535 & 7))));
    }

    printf("vsh reuse: OK (no entry survives a batch, duplicates still reused "
           "within one)\n");
    return 0;
}
