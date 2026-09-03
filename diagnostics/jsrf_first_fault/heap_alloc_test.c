/*
 * heap_alloc_test.c - guest heap allocator regression test
 *
 * JSRF exhausted the 48 MB guest heap while loading D:\Media\Stage\Stg10_01.dat.
 * The measured cause was not fragmentation and not a title that wants more RAM
 * than the console has: nothing was ever returned to the heap. At the first
 * failure there were 997 live blocks and zero free ones, because every kernel
 * free the title called was routed somewhere that could not release a guest
 * block. Repairing those exposed a second failure -- 13.9 MB free, a 2.8 MB
 * largest free block, and a 349 KB page-aligned request failing anyway --
 * because a block split at the end of the request left the remainder at an
 * address no page-aligned request could use.
 *
 * These are the allocator behaviours those two repairs depend on. They are
 * checked through the real xbox_HeapAlloc/xbox_HeapFree against a live memory
 * layout, not a reimplementation, because the bugs were in the bookkeeping
 * rather than in the idea.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xbox_memory_layout.h"

/* The kernel library reaches into the recompiled image and the video pump for
 * work this test never triggers. Nothing here runs guest code or plays a
 * video, so the honest stand-ins are "no such function" and "nothing playing";
 * linking the generated image in would make a heap test depend on a full
 * regeneration. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* The smallest XBE the memory layout will accept: a header whose section
 * count is zero, so nothing is copied into guest memory and the run is
 * entirely about the heap that sits above it. */
static uint8_t g_xbe[0x400];

static void build_xbe(void)
{
    memset(g_xbe, 0, sizeof g_xbe);
    memcpy(g_xbe, "XBEH", 4);
    *(uint32_t *)(g_xbe + 0x0104) = 0x00010000u;   /* base address   */
    *(uint32_t *)(g_xbe + 0x0108) = sizeof g_xbe;  /* header size    */
    *(uint32_t *)(g_xbe + 0x011C) = 0;             /* section count  */
    *(uint32_t *)(g_xbe + 0x0120) = 0x00010000u;   /* section headers */
}

int main(void)
{
    uint32_t a, b, c, d, big;

    build_xbe();
    if (!xbox_MemoryLayoutInit(g_xbe, sizeof g_xbe)) {
        fprintf(stderr, "FAIL: memory layout would not initialise\n");
        return 1;
    }

    /* A freed block comes back. This is the whole point: before the repair the
     * table was matched and marked, but no kernel export reached it. */
    a = xbox_HeapAlloc(0x10000, 4096);
    check(a != 0, "first allocation");
    xbox_HeapFree(a);
    b = xbox_HeapAlloc(0x10000, 4096);
    check(b == a, "a freed block is handed out again");

    /* The physical-memory mirror names the same RAM. A title that allocates
     * through the ordinary address and frees through 0x80000000 + address is
     * freeing its own block, which is what MmFreeContiguousMemory does. */
    xbox_HeapFree(b | 0x80000000u);
    c = xbox_HeapAlloc(0x10000, 4096);
    check(c == a, "a free through the physical mirror releases the block");

    /* Splitting: a small request served from a large free block must leave the
     * remainder allocatable, and page-aligned, or a later aligned request
     * fails with megabytes free. */
    xbox_HeapFree(c);
    d = xbox_HeapAlloc(0x1000, 4096);
    check(d == a, "small request reuses the large free block");
    big = xbox_HeapAlloc(0xE000, 4096);
    check(big == a + 0x1000, "the remainder is reused, page-aligned");
    check((big & 0xFFFu) == 0, "the remainder starts on a page boundary");

    /* An unaligned request must not leave an unusable remainder either. */
    xbox_HeapFree(big);
    xbox_HeapFree(d);
    d = xbox_HeapAlloc(0x1234, 4096);
    check(d == a, "unaligned-size request reuses the block");
    big = xbox_HeapAlloc(0x8000, 4096);
    check(big == a + 0x2000, "remainder after an unaligned request is aligned");

    /* Coalescing has to survive its own bookkeeping. Merging leaves a dead
     * table entry behind, and the neighbour test used to look only at the
     * adjacent index -- so two adjacent free blocks either side of a dead
     * entry never merged, and the heap fragmented permanently. Freeing these
     * three in an order that produces a hole must still yield one block big
     * enough for all of them. */
    xbox_HeapFree(big);
    xbox_HeapFree(d);
    a = xbox_HeapAlloc(0x4000, 4096);
    b = xbox_HeapAlloc(0x4000, 4096);
    c = xbox_HeapAlloc(0x4000, 4096);
    d = xbox_HeapAlloc(0x4000, 4096);
    check(a && b == a + 0x4000 && c == b + 0x4000 && d == c + 0x4000,
          "four adjacent blocks");
    xbox_HeapFree(a);
    xbox_HeapFree(b);   /* merges into a, leaving b's entry dead */
    xbox_HeapFree(c);   /* must merge across the dead entry */
    xbox_HeapFree(d);
    big = xbox_HeapAlloc(0x10000, 4096);
    check(big == a, "all four coalesce back into one block");

    /* Reported sizes follow the block, through the mirror as well. */
    check(xbox_HeapBlockSize(big) == 0x10000, "block size");
    check(xbox_HeapBlockSize(big + 0x100) == 0x10000 - 0x100,
          "interior block size");
    check(xbox_HeapBlockSize(big | 0x80000000u) == 0x10000,
          "block size through the physical mirror");

    /* A free of something this heap never issued is not a match and must not
     * corrupt the table. */
    xbox_HeapFree(0x00000040u);
    xbox_HeapFree(big);
    check(xbox_HeapAlloc(0x10000, 4096) == big, "table survives a stray free");

    if (failures) {
        fprintf(stderr, "%d heap allocator check(s) failed\n", failures);
        return 1;
    }
    printf("heap allocator: all checks passed\n");
    return 0;
}
