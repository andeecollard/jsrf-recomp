/* The scanner is only worth having if it names the right object, so the test
 * builds the crash it is meant to explain rather than a generic one.
 *
 * The shape comes from the real report of 19 Sep 2026: sub_00011D00,
 * CActBase::recursiveExec1Default, died on MEM32(edi+4) with EDI = ECX =
 * 0xFF555555 on the first iteration of a recursive call. Its generated body
 * begins PUSH32(esp,ecx); PUSH32(esp,ebp); ebp = ecx, and recurses with
 * ecx = MEM32(edi+0x28) -- so the parent's own `this` is still on the guest
 * stack when the child faults, and the parent's +0x28 still holds the value
 * that killed it. Those two facts are what the scanner exploits, and if either
 * ever stops being true this test is where it should show.
 *
 * The second half is the discrimination the crash actually turns on.
 * 0xFF555555 is opaque mid-grey in ARGB8888 and is exactly the DXT1 one-third
 * interpolant between black and white, so "the walk followed a pointer into
 * colour data" is a live hypothesis. A corrupted pointer field is one isolated
 * word; a colour buffer is thousands in a row. The run length separates them,
 * so the run length is tested.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wild_ptr.h"

static int fails;

static void ok(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-52s %ld\n", what, got);
    else { printf("  FAIL %-52s got %ld, want %ld\n", what, got, want); ++fails; }
}

/* The image is a flat little-endian byte array indexed by guest VA, which is
 * precisely what the runtime hands the scanner at a crash. */
#define IMG_SIZE 0x00100000u

static unsigned char *img;

static void put32(uint32_t va, uint32_t v)
{
    img[va]     = (unsigned char)(v);
    img[va + 1] = (unsigned char)(v >> 8);
    img[va + 2] = (unsigned char)(v >> 16);
    img[va + 3] = (unsigned char)(v >> 24);
}

/* CActBase, per the offsets already used by RECOMP_OBJECT_DUMP in main.c:
 * +00 vtable, +04 eACTFLAG, +08 id, +0C draw child mask, +24 parent,
 * +28 child, +2C/+30 siblings. */
static void put_node(uint32_t va, uint32_t vtable, uint32_t id,
                     uint32_t parent, uint32_t child, uint32_t sibling)
{
    put32(va + 0x00, vtable);
    put32(va + 0x04, 0x00000001u);
    put32(va + 0x08, id);
    put32(va + 0x24, parent);
    put32(va + 0x28, child);
    put32(va + 0x30, sibling);
}

#define WILD   0xFF555555u
#define RAM_LO 0x00010000u
#define RAM_HI IMG_SIZE

int main(void)
{
    JsrfWpParent parents[8];
    JsrfWpHit hits[8];
    uint32_t regs[JSRF_WP_NREG];
    uint32_t total, runs, longest, n;
    int reg = -1;
    uint32_t disp = 0;

    img = (unsigned char *)calloc(1, IMG_SIZE);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }

    /* ---- register attribution, with the registers the crash really had ---- */
    regs[JSRF_WP_EAX] = 0x00000000u;
    regs[JSRF_WP_ECX] = 0xFF555555u;
    regs[JSRF_WP_EDX] = 0x3F800000u;
    regs[JSRF_WP_EBX] = 0x00FFFFFFu;
    regs[JSRF_WP_ESI] = 0x80000000u;
    regs[JSRF_WP_EDI] = 0xFF555555u;
    regs[JSRF_WP_EBP] = 0x0050FDE4u;
    regs[JSRF_WP_ESP] = 0x0050FD6Cu;

    /* 0xFF555559 is the guest VA implied by the reported host fault address
     * 0x70FF555559. The read was MEM32(edi + 4). */
    ok("attributes the fault to a register",
       jsrf_wp_attribute(0xFF555559u, regs, 0x1000u, &reg, &disp), 1);
    /* ECX holds the same garbage and comes first in the register file, so a
     * scan that stopped at the first match would name ECX. It is a tie on
     * displacement, and ties go to register order, so ECX is the right answer
     * here -- what matters is that the DISPLACEMENT is 4 either way, because
     * that is the number that identifies the field. */
    ok("  ...at displacement 4", disp, 4);
    ok("  ...to a register holding the wild value",
       reg >= 0 ? (long)regs[reg] : -1, (long)WILD);

    /* EBP is 0x0050FDE4 and the fault is nowhere near it, so a tight window
     * must not sweep it in. */
    ok("a far register is not attributed",
       jsrf_wp_attribute(0x0050F000u, regs, 0x100u, NULL, NULL), 0);
    /* Negative control: an address no register can reach as base+displacement
     * must be attributed to nothing at all. */
    ok("an unreachable address is attributed to nothing",
       jsrf_wp_attribute(0x00400000u, regs, 0x1000u, NULL, NULL), 0);
    /* EAX was zero at this crash, so low addresses genuinely ARE reachable
     * from it. Recording that here because it is the one case where a match
     * is not evidence of much, and a reader of the report should know the
     * scan behaves this way rather than discovering it mid-diagnosis. */
    ok("a null base still matches low addresses",
       jsrf_wp_attribute(0x00000010u, regs, 0x1000u, &reg, &disp), 1);
    ok("  ...naming EAX", reg, JSRF_WP_EAX);
    reg = -1;

    /* ---- the walk: grandparent -> parent -> (corrupt child) ---- */
    {
        const uint32_t grandparent = 0x00020000u;
        const uint32_t parent      = 0x00021000u;
        const uint32_t stack_top   = 0x000A0000u;
        const uint32_t esp         = stack_top - 0x40u;

        put_node(grandparent, 0x0036C000u, 1, 0, parent, 0);
        put_node(parent,      0x0036C040u, 2, grandparent, 0, 0);
        /* The one bad field. This is what the walker read and recursed on. */
        put32(parent + 0x28, WILD);

        /* The guest stack as sub_00011D00 leaves it: the recursive call
         * pushed a return address, then the callee pushed ecx (the corrupt
         * child), ebp (the PARENT -- the datum the whole scan is after), edi,
         * ebx and esi. */
        put32(esp + 0x00, 0x00000000u);      /* saved esi */
        put32(esp + 0x04, 0x00FFFFFFu);      /* saved ebx */
        put32(esp + 0x08, grandparent);      /* saved edi */
        put32(esp + 0x0C, parent);           /* saved ebp == parent's this */
        put32(esp + 0x10, WILD);             /* saved ecx == the bad child */
        put32(esp + 0x14, 0x00011D8Du);      /* return into the walker */

        n = jsrf_wp_parents(img, esp, stack_top, RAM_LO, RAM_HI, WILD,
                            0x100u, parents, 8);
        ok("finds exactly one holder on the stack", n, 1);
        ok("  ...and it is the parent node", parents[0].node, parent);
        ok("  ...at field +0x28", parents[0].field, 0x28);
        ok("  ...recovered from the saved-ebp slot", parents[0].via, esp + 0x0C);

        /* The grandparent is on the stack too and does NOT hold the value.
         * If it were reported the scan would be matching on reachability
         * rather than on content, which would name an innocent object in
         * every future crash. */
        {
            uint32_t i, saw_grandparent = 0;
            for (i = 0; i < n && i < 8; ++i)
                if (parents[i].node == grandparent) saw_grandparent = 1;
            ok("does not implicate the innocent grandparent", saw_grandparent, 0);
        }

        /* Negative control on the value: a node graph that holds no wild
         * pointer must produce no parents at all. */
        ok("a value nothing holds yields no parent",
           jsrf_wp_parents(img, esp, stack_top, RAM_LO, RAM_HI, 0xDEADBE55u,
                           0x100u, parents, 8), 0);

        /* ---- isolated corruption versus a colour buffer ---- */
        total = jsrf_wp_scan(img, RAM_LO, RAM_HI, WILD, hits, 8,
                             &runs, &longest);
        /* Two words: the parent's +0x28 and the saved ecx on the stack. */
        ok("scan finds the corruption and its copy", total, 2);
        ok("  ...as two separate runs", runs, 2);
        ok("  ...neither longer than one word", longest, 1);
        ok("  ...first hit is the parent's field", hits[0].va, parent + 0x28);

        /* Now make it look like decoded colour instead: 4096 identical texels.
         * Same value, completely different verdict, and the difference is
         * visible without knowing anything about the graph. */
        {
            uint32_t va, surface = 0x00040000u;
            for (va = 0; va < 4096u * 4u; va += 4u)
                put32(surface + va, WILD);
            total = jsrf_wp_scan(img, RAM_LO, RAM_HI, WILD, hits, 8,
                                 &runs, &longest);
            ok("a colour buffer reads as one long run", longest, 4096);
            ok("  ...and lifts the total far above the pointer case",
               total, 4096 + 2);
            ok("  ...while the number of runs stays small", runs, 3);
        }
    }

    /* ---- the scan's own bounds ---- */
    ok("an empty range scans nothing",
       jsrf_wp_scan(img, 0x1000u, 0x1000u, WILD, NULL, 0, NULL, NULL), 0);
    ok("an inverted range scans nothing",
       jsrf_wp_scan(img, 0x2000u, 0x1000u, WILD, NULL, 0, NULL, NULL), 0);
    ok("a NULL image scans nothing",
       jsrf_wp_scan(NULL, RAM_LO, RAM_HI, WILD, NULL, 0, NULL, NULL), 0);
    ok("a NULL image yields no parents",
       jsrf_wp_parents(NULL, 0x1000u, 0x2000u, RAM_LO, RAM_HI, WILD,
                       0x100u, NULL, 0), 0);
    /* Counting must not depend on having somewhere to put the hits: the
     * total is the measurement, the hit list is only an illustration. */
    ok("counts correctly with no output buffer",
       jsrf_wp_scan(img, RAM_LO, RAM_HI, WILD, NULL, 0, NULL, NULL),
       4096 + 2);

    free(img);
    printf(fails ? "\nFAILED (%d)\n" : "\nOK\n", fails);
    return fails ? 1 : 0;
}
