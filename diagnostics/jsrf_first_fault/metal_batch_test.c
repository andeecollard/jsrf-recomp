/* Several draws, one readback: the case batching actually changes.
 *
 * WHY metal_copy_test DOES NOT COVER THIS. Every comparison there syncs
 * immediately after its single draw, so the batch is one draw long and the
 * encoder is closed before the next one opens. Running it with
 * RECOMP_METAL_BATCH=1 therefore proves the flush happens, and nothing about
 * what batching is for: several draws accumulated into ONE render encoder.
 *
 * That case has a hazard the per-draw path did not. The fragment shader
 * implements the guest's blending, depth test and stencil itself, by reading
 * the attachments back -- so two triangles covering the same pixel are a
 * read-modify-write pair, and they have to happen in the order the guest
 * emitted them. Committing a command buffer per draw enforced that by brute
 * force. Inside one encoder the ordering comes instead from the attachments
 * being declared raster_order_group(0), and this test is the evidence that the
 * declaration does the job -- for overlapping geometry, with blending and the
 * depth test live, which is where a missing barrier would show.
 *
 * WHAT IS COMPARED, and why it is not the software rasteriser. The obvious
 * oracle is nv2a_texture_copy_*, and this does run it -- but it cannot be the
 * gate, because the per-draw path DISAGREES WITH IT TOO, on 4 of 1024 pixels,
 * before any of this was written. Failing the batched run for a difference the
 * unbatched run also has would be blaming batching for something older. (That
 * disagreement is real and worth its own investigation; so is the depth
 * mismatch metal_copy_test reports at its line 49. Neither is this change.)
 *
 * So the gate is the direct one: the image the per-draw path produces, byte for
 * byte, against the image the batched path produces from the same draws. That
 * is exactly the question "does batching change what is drawn", it needs no
 * oracle to be perfect, and it is the comparison to make before any timing.
 * The oracle count is still reported, and must MATCH between the two runs --
 * a batched run that agreed with the per-draw image but drifted from the
 * rasteriser differently would mean the comparison had not run what it thought.
 *
 * Run twice, because the switch is read once per process:
 *     jsrf_metal_batch_test  a.bin
 *     RECOMP_METAL_BATCH=1 jsrf_metal_batch_test  b.bin
 *     cmp a.bin b.bin
 * metal_batch_check.sh does that and reports it.
 */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)

#define W 32u
#define H 32u
#define DRAWS 40u
#define TARGET_BYTES (W*H*2u)
#define DEPTH_BYTES  (W*H*4u)

/* A fixed generator, so a failure is reproducible and the two registrations
 * see byte-identical geometry. */
static unsigned rng_state = 0x1234567u;
static unsigned rnd(unsigned n){rng_state=rng_state*1103515245u+12345u;return (rng_state>>16)%n;}

int main(int argc, char **argv)
{
    static uint8_t tex[512];
    static uint8_t cpu[TARGET_BYTES], gpu[TARGET_BYTES];
    static uint8_t zcpu[DEPTH_BYTES], zgpu[DEPTH_BYTES];
    const char *mode = getenv("RECOMP_METAL_BATCH") ? "batched" : "per-draw";
    unsigned d, i, drawn = 0;

    for (i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73 + 11);

    /* Both surfaces start identical and neither is touched again by the host
     * until the single readback at the end. */
    memset(cpu, 0xcc, sizeof cpu); memcpy(gpu, cpu, sizeof gpu);
    for (i = 0; i < DEPTH_BYTES; i += 4) {
        zcpu[i] = 0x5a; zcpu[i+1] = 0xff; zcpu[i+2] = 0xff; zcpu[i+3] = 0xff;
    }
    memcpy(zgpu, zcpu, sizeof zgpu);
    nv2a_metal_invalidate(gpu);

    for (d = 0; d < DRAWS; ++d) {
        NV2ATextureCopy s = {0};
        float v[3][16][4] = {{{0}}};
        unsigned j;
        s.width = s.height = 16; s.pitch = 32; s.levels = 1; s.texture_mask = 1;
        s.clip_w = W; s.clip_h = H; s.target_pitch = W * 2; s.target_bpp = 2;
        s.depth_pitch = W * 4;
        /* Depth test and write on for most draws, so later triangles are
         * rejected by earlier ones -- the ordering-sensitive case. Blending on
         * for the rest, which is the other read-modify-write of the colour
         * attachment. */
        if (d & 1) { s.depth_test = 1; s.depth_write = 1; s.depth_func = 4; }
        else       { s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303; }
        s.linear = d & 2 ? 1 : 0;

        /* Deliberately overlapping: every triangle is anchored near the middle
         * so the same pixels are written over and over. */
        for (j = 0; j < 3; ++j) {
            v[j][0][0] = (float)(4 + rnd(W - 8));
            v[j][0][1] = (float)(4 + rnd(H - 8));
            v[j][0][2] = (float)(rnd(0x1000000));
            v[j][0][3] = 1.0f;                       /* w */
            v[j][3][0] = (float)rnd(256) / 255.0f;   /* diffuse */
            v[j][3][1] = (float)rnd(256) / 255.0f;
            v[j][3][2] = (float)rnd(256) / 255.0f;
            v[j][3][3] = (float)rnd(256) / 255.0f;
            v[j][9][0] = (float)rnd(16);             /* texcoord0 */
            v[j][9][1] = (float)rnd(16);
            v[j][9][3] = 1.0f;
        }

        /* The oracle first, then the GPU, with NO sync in between -- that
         * absence is the entire point of this test. */
        if (!nv2a_texture_copy_triangle_depth(&s, tex, sizeof tex,
                                              cpu, sizeof cpu, zcpu, sizeof zcpu,
                                              v[0], v[1], v[2]))
            continue;                    /* degenerate: neither path draws it */
        {
            int r = nv2a_metal_draw(&s, tex, sizeof tex, gpu, sizeof gpu,
                                    zgpu, sizeof zgpu, v, 3, 5);
            if (r < 0) {
                fprintf(stderr, "draw %u rejected: %s\n", d, nv2a_metal_last_reject());
                return 1;
            }
            drawn += (unsigned)r;
        }
    }

    /* One readback for all of them. With batching on this is the first time
     * the GPU is told to run anything at all. */
    CHECK(nv2a_metal_sync());

    /* A positive control on the test itself: if nothing was rasterised, the
     * two buffers agree trivially and the comparison below proves nothing. */
    if (!drawn) { fprintf(stderr, "no triangles survived assembly\n"); return 1; }

    {
        unsigned bad = 0, zbad = 0;
        for (i = 0; i < sizeof cpu; i += 2)
            if (cpu[i] != gpu[i] || cpu[i+1] != gpu[i+1]) ++bad;
        for (i = 0; i < sizeof zcpu; ++i) if (zcpu[i] != zgpu[i]) ++zbad;
        printf("metal batch (%s): %u draws, %u triangles, one readback; "
               "oracle differs on %u of %u pixels, %u of %u depth bytes\n",
               mode, DRAWS, drawn, bad, W * H, zbad, (unsigned)sizeof zcpu);
    }

    /* The image itself, for the caller to compare against the other mode. */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (!f) { perror(argv[1]); return 1; }
        if (fwrite(gpu, 1, sizeof gpu, f) != sizeof gpu ||
            fwrite(zgpu, 1, sizeof zgpu, f) != sizeof zgpu) {
            fprintf(stderr, "short write to %s\n", argv[1]);
            fclose(f); return 1;
        }
        fclose(f);
    }
    return 0;
}
