/* The staging ring must not hand back memory the GPU is still reading.
 *
 * Batching changed how that guarantee is implemented. The per-draw path pinned
 * a slab to every command buffer; a batch now pins each slab ONCE, from a
 * bitmask, to the one command buffer covering many draws. The in-flight count
 * therefore counts command buffers rather than draws, which is correct only if
 * the bitmask really catches every slab the batch touched.
 *
 * That was argued in a comment and never tested. The comment cited a harness
 * driven through 20,000 blit command buffers -- but that harness was never
 * committed, so the claim could not be re-run when the thing it was about
 * changed. This is that test, kept, and driving the real ring_reserve and
 * ring_pin through nv2a_metal_ring_selftest rather than a copy of them.
 *
 * The control is the whole point: mode 0 pins nothing, and MUST corrupt. A
 * ring test that cannot report corruption is measuring nothing.
 */
#include "nv2a_metal.h"
#include <stdio.h>

int main(void)
{
    struct { int mode; const char *what; int must_corrupt; } cases[] = {
        { 0, "no pinning at all (positive control)",        1 },
        { 1, "pinned per command buffer (the per-draw path)", 0 },
        { 2, "pinned once per batch (what batching does)",  0 },
    };
    unsigned i, failures = 0;
    /* Two slabs, so the ring comes round every eight reservations and the GPU
     * is still working when it does. Eight slabs takes long enough to wrap
     * that the hazard is never reached, which is how an earlier version of
     * this check passed while covering nothing. */
    const unsigned SLABS = 2, ITERS = 4000, PER_BATCH = 16;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        unsigned corrupt = 0;
        if (!nv2a_metal_ring_selftest(SLABS, cases[i].mode, ITERS, PER_BATCH, &corrupt)) {
            fprintf(stderr, "could not run: %s\n", cases[i].what);
            return 1;
        }
        if (corrupt == 0xFFFFFFFFu) {
            fprintf(stderr, "harness failure during: %s\n", cases[i].what);
            return 1;
        }
        printf("  mode %d  %-42s %u of %u reservations corrupted\n",
               cases[i].mode, cases[i].what, corrupt, ITERS);
        if (cases[i].must_corrupt && corrupt == 0) {
            fprintf(stderr, "  ^ CONTROL DID NOT FIRE: with no pinning at all the "
                            "ring must hand back memory the GPU is still reading. "
                            "It did not, so this test proves nothing about the two "
                            "modes below it.\n");
            ++failures;
        }
        if (!cases[i].must_corrupt && corrupt != 0) {
            fprintf(stderr, "  ^ FAILED: staging memory was reused while in flight\n");
            ++failures;
        }
    }
    if (failures) return 1;
    printf("metal ring: pinning protects staging memory, per command buffer "
           "and per batch; removing it corrupts, so the test has teeth\n");
    return 0;
}
