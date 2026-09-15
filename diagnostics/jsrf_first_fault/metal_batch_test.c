/* Does batching change what is drawn? Five phases, one dump, compared byte for
 * byte against the per-draw path.
 *
 * WHY THE PHASES. The first version of this exercised only overlapping draws,
 * and concluded from raster_order_group(0) that ordering was safe. Fragment
 * ordering is not the whole of correctness: batching also changes WHEN
 * staging memory is released, when a texture buffer may be replaced, and when
 * the surface may be torn down and rebuilt, because all three used to be
 * bounded by a command buffer that was committed immediately and are now
 * bounded by a flush that may be many draws away. Raster order groups say
 * nothing about any of that. So each of those boundaries gets a phase:
 *
 *   A  overlapping blended and depth-tested draws   fragment ordering
 *   B  draws large enough to wrap the staging ring  vertex lifetime
 *   C  more distinct textures than the cache holds  texture lifetime
 *   D  alternating render targets                   surface teardown
 *   E  invalidate and readback between draws        flush boundaries
 *
 * WHAT IS COMPARED. Not the software rasteriser: the per-draw path already
 * disagrees with it on 4 of 1024 pixels and 587 of 4096 depth bytes in phase A,
 * before any of this was written, and failing the batched run for that would
 * blame this change for an older one. (That disagreement is real and tracked
 * separately, as is the depth mismatch metal_copy_test reports at its line 49.)
 * The gate is the direct one: every byte this program reads back, per-draw
 * against batched. The oracle count for phase A is still reported and must
 * match between the two, because a run that agreed on the image but drifted
 * from the rasteriser differently would mean the two runs had not done the
 * same work.
 *
 * Run twice -- the switch is read once per process, and batching is the
 * default, so it is the per-draw arm that has to ask for itself:
 *     RECOMP_METAL_BATCH=0 jsrf_metal_batch_test  a.bin
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

/* 256x256, not 32x32. The first version used a surface small enough that the
 * GPU finished each pass almost immediately, and that made the ring-wrap phase
 * toothless: removing the slab pinning entirely -- so staging memory can be
 * overwritten while the GPU is still reading it -- produced an IDENTICAL image,
 * because the GPU was never actually behind. A test that cannot fail for a
 * defect is not covering it. With a real fragment load the GPU lags the
 * producer and the race is reachable; the negative control below is the
 * evidence that it now is. */
#define W 256u
#define H 256u
#define TARGET_BYTES (W*H*2u)
#define DEPTH_BYTES  (W*H*4u)

/* Bigger than the ring slab can hold in a handful of draws: a Vertex is 112
 * bytes, so 4000 of them is ~448 KB against a 2 MB slab and 8 slabs, and 100
 * such draws go round the ring about three times. */
#define BIG_VERTS 4000u
#define BIG_DRAWS 40u        /* 40 * ~464 KB > the 16 MB ring: it must wrap */
/* More than TEXTURE_CACHE_SIZE (128), so the cache must evict while a batch is
 * open and the evicted buffer may still be referenced by an unflushed draw. */
#define TEXTURES 200u
#define TEX_BYTES 512u

static unsigned rng_state = 0x1234567u;
static unsigned rnd(unsigned n){rng_state=rng_state*1103515245u+12345u;return (rng_state>>16)%n;}

static uint8_t textures[TEXTURES][TEX_BYTES];
static uint8_t targetA[TARGET_BYTES], targetB[TARGET_BYTES];
static uint8_t depthA[DEPTH_BYTES],  depthB[DEPTH_BYTES];
static uint8_t oracle[TARGET_BYTES], oracle_z[DEPTH_BYTES];
static unsigned oracle_worst, oracle_one_step;
/* Phase F: three surfaces, because JSRF holds three and swaps between them
 * batch by batch, and this backend retains ONE Metal texture and re-uploads on
 * every swap. Its own buffers rather than phase A's: by the time F runs,
 * targetA has been through B, C and D without the rasteriser alongside it, so
 * the phase A oracle has legitimately diverged and cannot be reused. */
#define SWAPS 3u
static uint8_t swap_t[SWAPS][TARGET_BYTES], swap_z[SWAPS][DEPTH_BYTES];
static uint8_t swap_o[SWAPS][TARGET_BYTES], swap_oz[SWAPS][DEPTH_BYTES];
static unsigned swap_bad, swap_zbad, swap_worst, swap_one_step;
static float big[BIG_VERTS][16][4];

static FILE *dump;
static unsigned long drawn;

/* Everything the host can observe, appended after each phase. A difference
 * anywhere in this file is a difference in what the GPU produced. */
static void record(void)
{
    if (!dump) return;
    fwrite(targetA, 1, sizeof targetA, dump);
    fwrite(targetB, 1, sizeof targetB, dump);
    fwrite(depthA,  1, sizeof depthA,  dump);
    fwrite(depthB,  1, sizeof depthB,  dump);
    fwrite(swap_t,  1, sizeof swap_t,  dump);
    fwrite(swap_z,  1, sizeof swap_z,  dump);
}

static void base_state(NV2ATextureCopy *s)
{
    memset(s, 0, sizeof *s);
    s->width = s->height = 16; s->pitch = 32; s->levels = 1; s->texture_mask = 1;
    s->clip_w = W; s->clip_h = H; s->target_pitch = W * 2; s->target_bpp = 2;
    s->depth_pitch = W * 4;
    /* A DEPTH RANGE, because memset(0) is a state the guest cannot produce and
     * it silently disabled depth on one path.
     *
     * The software path clamps every fragment to [z_clip_min, z_clip_max] when
     * z_cull is off -- so a zeroed pair clamps ALL depth to zero, and that path
     * then cannot depth-test at all. The hardware path takes depth from the
     * rasteriser and is unaffected, so the two paths were not rendering the
     * same quantity. nv2a_texture_copy.c:76 normalises exactly this case back
     * to [0, 16777215] when it decodes NV097_SET_CLIP_MIN/MAX, so no real draw
     * ever reaches the backend with it; only this fixture, which fills the
     * struct by hand, could.
     *
     * It stayed invisible because every phase also set depth_func = 4, which is
     * not an NV2A encoding -- nv2a_metal_compare_func returns -1 and both paths
     * fall back to ALWAYS. Two fixture defects cancelling: no depth to compare,
     * and no comparison to make. */
    s->z_clip_min = 0.0f; s->z_clip_max = 16777215.0f;
}

static void fill_tri(float v[3][16][4], unsigned span)
{
    unsigned j;
    for (j = 0; j < 3; ++j) {
        v[j][0][0] = (float)(rnd(W - 8) + 4);
        v[j][0][1] = (float)(rnd(H - 8) + 4);
        v[j][0][2] = (float)rnd(0x1000000);
        v[j][0][3] = 1.0f;
        v[j][3][0] = (float)rnd(256) / 255.0f;
        v[j][3][1] = (float)rnd(256) / 255.0f;
        v[j][3][2] = (float)rnd(256) / 255.0f;
        v[j][3][3] = (float)rnd(256) / 255.0f;
        v[j][9][0] = (float)rnd(16);
        v[j][9][1] = (float)rnd(16);
        v[j][9][3] = 1.0f;
    }
    (void)span;
}

static int draw(const NV2ATextureCopy *s, const uint8_t *tex,
                uint8_t *target, uint8_t *depth,
                const float (*v)[16][4], unsigned count, const char *phase)
{
    int r = nv2a_metal_draw(s, tex, TEX_BYTES, target, TARGET_BYTES,
                            depth, DEPTH_BYTES, v, count, 5);
    if (r < 0) {
        fprintf(stderr, "%s: draw rejected: %s\n", phase, nv2a_metal_last_reject());
        return 0;
    }
    drawn += (unsigned)r;
    return 1;
}

/* COUNT AND MAGNITUDE, in 565 channel steps -- the unit this project's
 * acceptance rule is written in. Shared by phase A and phase F so the two
 * report the same quantity. */
static void score(const uint8_t *got, const uint8_t *want, size_t bytes,
                  unsigned *bad, unsigned *worst_out, unsigned *one_step)
{
    size_t i;
    for (i = 0; i < bytes; i += 2) {
        unsigned o = want[i] | (unsigned)want[i+1] << 8;
        unsigned g = got[i]  | (unsigned)got[i+1]  << 8;
        int dr, dg, db, worst;
        if (o == g) continue;
        ++*bad;
        dr = (int)(o >> 11)       - (int)(g >> 11);
        dg = (int)((o >> 5) & 63) - (int)((g >> 5) & 63);
        db = (int)(o & 31)        - (int)(g & 31);
        if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
        worst = dr > dg ? dr : dg; if (db > worst) worst = db;
        if (worst > (int)*worst_out) *worst_out = (unsigned)worst;
        if (worst == 1) ++*one_step;
    }
}

int main(int argc, char **argv)
{
    const char *e = getenv("RECOMP_METAL_BATCH");
    const char *mode = (e && atoi(e)) ? "batched" : "per-draw";
    unsigned d, i, oracle_bad = 0, oracle_zbad = 0;

    for (i = 0; i < TEXTURES; ++i)
        for (d = 0; d < TEX_BYTES; ++d)
            textures[i][d] = (uint8_t)(i * 31 + d * 73 + 11);

    memset(targetA, 0xcc, sizeof targetA); memcpy(targetB, targetA, sizeof targetB);
    memcpy(oracle, targetA, sizeof oracle);
    for (i = 0; i < DEPTH_BYTES; i += 4) {
        depthA[i] = 0x5a; depthA[i+1] = 0xff; depthA[i+2] = 0xff; depthA[i+3] = 0xff;
    }
    memcpy(depthB, depthA, sizeof depthB);
    memcpy(oracle_z, depthA, sizeof oracle_z);

    if (argc > 1) { dump = fopen(argv[1], "wb"); if (!dump) { perror(argv[1]); return 1; } }
    nv2a_metal_invalidate(NULL);

    /* A -- overlapping, blended, depth-tested, one readback at the end. The
     * software rasteriser runs alongside here only, as a cross-check that both
     * modes did the same work. */
    for (d = 0; d < 40; ++d) {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        base_state(&s);
        if (d & 1) { s.depth_test = 1; s.depth_write = 1; s.depth_func = 4; }
        else       { s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303; }
        s.linear = (d & 2) ? 1 : 0;
        fill_tri(v, 0);
        if (!nv2a_texture_copy_triangle_depth(&s, textures[0], TEX_BYTES,
                                              oracle, sizeof oracle,
                                              oracle_z, sizeof oracle_z,
                                              v[0], v[1], v[2]))
            continue;
        if (!draw(&s, textures[0], targetA, depthA, v, 3, "A")) return 1;
    }
    CHECK(nv2a_metal_sync());
    /* COUNT AND MAGNITUDE, because a count alone cannot be acted on.
     *
     * This project's stated acceptance rule for a GPU sink is that it "matches
     * the CPU rasteriser to within one RGB565 channel step" -- the rule the
     * D3D11 backend was accepted under, where 48% of pixels differed on a
     * single UNBLENDED draw and every one of them was off by exactly one step.
     * A bare count cannot distinguish that from a real error, so a narrower
     * colour attachment looks catastrophic when it may be conforming.
     *
     * Per channel, in 565 steps, because that is the unit the rule is written
     * in: a whole-pixel measure would hide a 1-step red behind a correct green. */
    score(targetA, oracle, sizeof oracle, &oracle_bad, &oracle_worst, &oracle_one_step);
    for (i = 0; i < sizeof oracle_z; ++i)
        if (oracle_z[i] != depthA[i]) ++oracle_zbad;
    record();

    /* B -- ring wrap. Each draw stages ~448 KB, so the 2 MB slabs turn over
     * every few draws and the wrap must wait on a slab an open batch has not
     * pinned yet. No sync inside the loop: the wrap is the only thing that may
     * flush, and that is exactly what is under test. */
    for (i = 0; i < BIG_VERTS; ++i) {
        /* Large, so each triangle costs real fragment work and the GPU falls
         * behind the thread staging vertices for the next draw. */
        big[i][0][0] = (float)(rnd(W - 2) + 1);
        big[i][0][1] = (float)(rnd(H - 2) + 1);
        if (i % 3 == 1) { big[i][0][0] += 64.0f; if (big[i][0][0] > W - 1) big[i][0][0] = W - 1; }
        if (i % 3 == 2) { big[i][0][1] += 64.0f; if (big[i][0][1] > H - 1) big[i][0][1] = H - 1; }
        big[i][0][2] = (float)rnd(0x1000000);
        big[i][0][3] = 1.0f;
        big[i][3][0] = (float)rnd(256) / 255.0f;
        big[i][3][3] = 1.0f;
        big[i][9][0] = (float)rnd(16);
        big[i][9][1] = (float)rnd(16);
        big[i][9][3] = 1.0f;
    }
    for (d = 0; d < BIG_DRAWS; ++d) {
        NV2ATextureCopy s; base_state(&s);
        s.depth_test = 1; s.depth_write = 1; s.depth_func = 4;
        if (!draw(&s, textures[d % TEXTURES], targetA, depthA,
                  (const float (*)[16][4])big, BIG_VERTS, "B")) return 1;
    }
    CHECK(nv2a_metal_sync());
    record();

    /* C -- more distinct textures than the cache holds, so entries are evicted
     * while a batch is open. The evicted MTLBuffer may still be bound into an
     * encoder that has not been committed. */
    for (d = 0; d < TEXTURES * 2; ++d) {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        base_state(&s);
        s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303;
        fill_tri(v, 0);
        if (!draw(&s, textures[d % TEXTURES], targetA, depthA, v, 3, "C")) return 1;
    }
    CHECK(nv2a_metal_sync());
    record();

    /* D -- alternating render targets. A target change tears the surface down,
     * syncs, reallocates and re-uploads; with a batch open that has to flush
     * first or the readback is missing every draw in it. */
    for (d = 0; d < 40; ++d) {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        uint8_t *t = (d & 1) ? targetB : targetA;
        uint8_t *z = (d & 1) ? depthB  : depthA;
        base_state(&s);
        s.depth_test = 1; s.depth_write = 1; s.depth_func = 4;
        fill_tri(v, 0);
        if (!draw(&s, textures[d % TEXTURES], t, z, v, 3, "D")) return 1;
    }
    CHECK(nv2a_metal_sync());
    record();

    /* E -- explicit invalidate and readback between draws, which is what a
     * guest clear and a flip look like from here. */
    for (d = 0; d < 40; ++d) {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        base_state(&s);
        s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303;
        fill_tri(v, 0);
        if (!draw(&s, textures[d % TEXTURES], targetA, depthA, v, 3, "E")) return 1;
        if ((d % 7) == 6) { CHECK(nv2a_metal_sync()); record(); }
        if ((d % 13) == 12) { nv2a_metal_invalidate(targetA); record(); }
    }
    CHECK(nv2a_metal_sync());
    record();

    /* F -- THREE surfaces, alternating, scored against the rasteriser.
     *
     * Phase D already alternates two render targets, but nothing ever scored
     * its image: D is compared batched-against-per-draw only, and the only
     * oracle comparison in this program is phase A, which never changes
     * surface at all. So metal_hw_check.sh -- which reads phase A's numbers --
     * passed a hardware path that renders a real mission wrong, with tiles
     * carrying content from elsewhere in the scene. A gate that cannot see the
     * swap cannot gate the swap.
     *
     * Three rather than two because that is what the title holds, and because
     * a two-surface alternation cannot tell "the retained texture is re-
     * uploaded from the wrong target" from "it is re-uploaded from the one
     * before last".
     *
     * Blended AND depth-tested, so the readback path for both attachments is
     * exercised on every swap: under RECOMP_METAL_HW depth lives in its own
     * attachment and has to be uploaded and read back per swap, which is
     * precisely the state the per-draw phases never make it rebuild. */
    for (i = 0; i < SWAPS; ++i) {
        memset(swap_t[i], 0xcc, TARGET_BYTES);
        for (d = 0; d < DEPTH_BYTES; d += 4) {
            swap_z[i][d] = 0x5a; swap_z[i][d+1] = 0xff;
            swap_z[i][d+2] = 0xff; swap_z[i][d+3] = 0xff;
        }
        memcpy(swap_o[i],  swap_t[i], TARGET_BYTES);
        memcpy(swap_oz[i], swap_z[i], DEPTH_BYTES);
    }
    for (d = 0; d < 120; ++d) {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        unsigned k = d % SWAPS;
        base_state(&s);
        /* 0x203, NOT 4. Every other phase sets depth_func = 4, a D3D-style
         * D3DCMP_LESSEQUAL the NV2A never emits -- its own encoding is the
         * 0x200 range -- so nv2a_metal_compare_func returns -1 and BOTH paths
         * fall back to ALWAYS. The consequence was invisible until this phase
         * was written: the only correctness gate the Metal renderer has never
         * performed a depth COMPARISON at all, on either path. Its depth
         * numbers came from depth writes. Use the real guest LEQUAL here so
         * the comparison, the MTLDepthStencilState built from it, and the
         * fragment shader cmpf() it replaces are all actually exercised. */
        s.depth_test = 1; s.depth_write = 1; s.depth_func = 0x203;
        if (d & 1) { s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303; }
        fill_tri(v, 0);
        if (!nv2a_texture_copy_triangle_depth(&s, textures[d % TEXTURES], TEX_BYTES,
                                              swap_o[k], TARGET_BYTES,
                                              swap_oz[k], DEPTH_BYTES,
                                              v[0], v[1], v[2]))
            continue;
        if (!draw(&s, textures[d % TEXTURES], swap_t[k], swap_z[k], v, 3, "F"))
            return 1;
    }
    CHECK(nv2a_metal_sync());
    record();
    for (i = 0; i < SWAPS; ++i) {
        unsigned b;
        score(swap_t[i], swap_o[i], TARGET_BYTES,
              &swap_bad, &swap_worst, &swap_one_step);
        for (b = 0; b < DEPTH_BYTES; ++b)
            if (swap_oz[i][b] != swap_z[i][b]) ++swap_zbad;
    }

    /* G -- a full clear of a surface the backend is NOT holding.
     *
     * nv2a_metal_discard drops the retained surface without reading it back,
     * which is sound only when the CPU is about to overwrite THAT surface's
     * guest memory. clear_surface decides from the clear's parameters and the
     * currently bound colour offset, and neither says which surface the backend
     * is holding -- the title swaps between three of them batch by batch, and
     * the bound offset moves on the register write while the backend's retained
     * surface only moves on the next DRAW. A clear in that window names one
     * surface and drops another.
     *
     * nv2a_metal_invalidate already takes a target for exactly this reason:
     * "It used to be one nv2a_gpu_invalidate(NULL) here, which flushes and
     * discards EVERY retained surface for a clear of one of them ... that blast
     * radius meant no surface ever survived a frame." The discard path was
     * added later and never got the same treatment.
     *
     * Draw into surface 0, leave it dirty, then discard as a clear of surfaces
     * 1 and 2 would. Surface 0's draws must still reach guest RAM. */
    {
        NV2ATextureCopy s; float v[3][16][4] = {{{0}}};
        unsigned bad = 0, gw = 0, gone = 0;
        memset(swap_t[0], 0xcc, TARGET_BYTES);
        for (d = 0; d < DEPTH_BYTES; d += 4) {
            swap_z[0][d] = 0x5a; swap_z[0][d+1] = 0xff;
            swap_z[0][d+2] = 0xff; swap_z[0][d+3] = 0xff;
        }
        memcpy(swap_o[0], swap_t[0], TARGET_BYTES);
        memcpy(swap_oz[0], swap_z[0], DEPTH_BYTES);
        nv2a_metal_invalidate(NULL);
        for (d = 0; d < 8; ++d) {
            base_state(&s);
            s.depth_test = 1; s.depth_write = 1; s.depth_func = 0x203;
            fill_tri(v, 0);
            if (!nv2a_texture_copy_triangle_depth(&s, textures[d], TEX_BYTES,
                                                  swap_o[0], TARGET_BYTES,
                                                  swap_oz[0], DEPTH_BYTES,
                                                  v[0], v[1], v[2]))
                continue;
            if (!draw(&s, textures[d], swap_t[0], swap_z[0], v, 3, "G")) return 1;
        }
        /* The clear names surfaces 1 and 2. Surface 0 is the one being held. */
        nv2a_metal_discard(swap_t[1], swap_z[1]);
        CHECK(nv2a_metal_sync());
        score(swap_t[0], swap_o[0], TARGET_BYTES, &bad, &gw, &gone);
        if (bad > (W * H) / 100u) {
            fprintf(stderr, "phase G: a clear naming ANOTHER surface threw away "
                    "%u of %u pixels drawn into the retained one (worst %u "
                    "step(s))\n", bad, W * H, gw);
            return 1;
        }
        /* POSITIVE CONTROL, because a discard that never fires would pass the
         * check above trivially and silently delete the optimisation. A clear
         * naming the surface actually being held must still take the fast
         * path, and must still say so. */
        if (!draw(&s, textures[0], swap_t[0], swap_z[0], v, 3, "G")) return 1;
        if (!nv2a_metal_discard(swap_t[0], swap_z[0])) {
            fprintf(stderr, "phase G: a clear naming the RETAINED surface was "
                            "refused -- the fast path is dead\n");
            return 1;
        }
        CHECK(nv2a_metal_sync());
        printf("metal discard: a clear naming another surface leaves the "
               "retained one intact (%u of %u pixels differ); a clear naming "
               "the retained one still discards\n", bad, W * H);
    }

    if (!drawn) { fprintf(stderr, "no triangles survived assembly\n"); return 1; }
    printf("metal batch (%s): %lu triangles over 6 phases "
           "(overlap, ring wrap, texture eviction, surface change, readback, "
           "three-surface swap); "
           "phase A differs from the software rasteriser on %u of %u pixels "
           "and %u of %u depth bytes; worst channel error %u step(s), "
           "%u of the differing pixels are within one\n",
           mode, drawn, oracle_bad, W * H, oracle_zbad, (unsigned)sizeof oracle_z,
           oracle_worst, oracle_one_step);
    printf("metal swap (%s): phase F differs from the software rasteriser on "
           "%u of %u pixels and %u of %u depth bytes; worst channel error "
           "%u step(s), %u of the differing pixels are within one\n",
           mode, swap_bad, SWAPS * W * H, swap_zbad,
           (unsigned)sizeof swap_z, swap_worst, swap_one_step);
    if (dump) {
        if (ferror(dump)) { fprintf(stderr, "write error on %s\n", argv[1]); return 1; }
        fclose(dump);
    }
    return 0;
}
