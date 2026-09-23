/* G27: does hardware texture sampling draw what the software sampler draws?
 *
 * Renders the same textured quads with RECOMP_METAL_HW_TEX off and on and
 * writes every target to a dump; metal_hwtex_check.sh runs both arms and
 * scores them with hwtex_compare.py. Byte equality is NOT the gate: the
 * software path filters in float from texel() while the hardware path
 * filters pre-decoded RGBA8 (nv2a_texture_decode.c rounds to 8 bits, and the
 * sampler's own LOD and weights are not the shader's arithmetic). The gate is
 * a small, stated tolerance on a 565 target, per case.
 *
 * Cases: DXT1, DXT3 and swizzled RGBA8 (0x06), 64x64 with a full mip chain;
 * each under point, bilinear, linear-mip-nearest and trilinear filtering,
 * clamp and repeat, drawn magnified, minified (so mips are taken) and with a
 * projective coordinate (tc.w = 2), and once more blended by source alpha so
 * texture alpha reaches the target. Needs a real GPU: built, not registered.
 */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256u
#define H 256u
#define TARGET_BYTES (W*H*2u)
#define DEPTH_BYTES  (W*H*4u)
#define TW 64u
#define LEVELS 7u
#define TEX_MAX (TW*TW*4u*2u)

static unsigned rng = 0x2718281u;
static unsigned rnd(void){rng=rng*1103515245u+12345u;return rng>>8;}
static uint8_t tex[3][TEX_MAX];
static size_t tex_bytes[3];
static uint8_t target[TARGET_BYTES], depth[DEPTH_BYTES];

/* Smooth colour fields with some hard edges, so filtering differences show. */
static void make_textures(void)
{
    for (int f = 0; f < 3; ++f) {
        size_t n = 0; unsigned w = TW, h = TW;
        for (unsigned l = 0; l < LEVELS; ++l) {
            size_t level = f == 0 ? (size_t)((w+3)/4)*((h+3)/4)*8
                         : f == 1 ? (size_t)((w+3)/4)*((h+3)/4)*16
                         : (size_t)w*h*4;
            for (size_t i = 0; i < level; ++i) {
                unsigned v = rnd();
                /* RGBA8: gradients by position in the level, noise in low bits */
                tex[f][n + i] = f == 2 ? (uint8_t)((i * 7 + l * 40 + (v & 15)) & 255)
                                       : (uint8_t)(v & 255);
            }
            n += level; w = w > 1 ? w/2 : 1; h = h > 1 ? h/2 : 1;
        }
        tex_bytes[f] = n;
    }
}

static void quad(float v[6][16][4], float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1, float q)
{
    const float P[6][4] = {{x0,y0,u0,v0},{x1,y0,u1,v0},{x1,y1,u1,v1},
                           {x0,y0,u0,v0},{x1,y1,u1,v1},{x0,y1,u0,v1}};
    memset(v, 0, sizeof(float) * 6 * 16 * 4);
    for (int j = 0; j < 6; ++j) {
        v[j][0][0] = P[j][0]; v[j][0][1] = P[j][1]; v[j][0][2] = 8388608.0f; v[j][0][3] = 1.0f;
        v[j][3][0] = v[j][3][1] = v[j][3][2] = v[j][3][3] = 1.0f;          /* white diffuse */
        v[j][9][0] = P[j][2] * q; v[j][9][1] = P[j][3] * q; v[j][9][3] = q; /* projective */
    }
}

int main(int argc, char **argv)
{
    FILE *dump = argc > 1 ? fopen(argv[1], "wb") : NULL;
    const unsigned mins[4] = {1, 2, 4, 6};
    unsigned cases = 0, rejected = 0;
    if (argc > 1 && !dump) { perror(argv[1]); return 1; }
    /* Positive control for the scorer: HWTEX_TEST_SEED changes the texture
     * content, and comparing two seeds must FAIL, or the gate proves nothing. */
    if (getenv("HWTEX_TEST_SEED")) rng = (unsigned)strtoul(getenv("HWTEX_TEST_SEED"), NULL, 0);
    make_textures();
    nv2a_metal_invalidate(NULL);

    for (int f = 0; f < 3; ++f)
    for (int m = 0; m < 4; ++m)
    for (int rep = 0; rep < 2; ++rep)
    for (int geo = 0; geo < 3; ++geo)
    for (int blend = 0; blend < 2; ++blend) {
        NV2ATextureCopy s; float v[6][16][4];
        memset(&s, 0, sizeof s);
        s.width = s.height = TW; s.levels = LEVELS; s.texture_mask = 1;
        s.dxt1 = f == 0; s.dxt3 = f == 1; s.rgba8 = f == 2;
        s.pitch = f == 0 ? (TW/4)*8 : f == 1 ? (TW/4)*16 : TW*4;
        s.min_filter = mins[m]; s.linear = m != 0; s.repeat = (unsigned)rep;
        s.modulate = 1;
        s.clip_w = W; s.clip_h = H; s.target_pitch = W*2; s.target_bpp = 2; s.depth_pitch = W*4;
        s.z_clip_min = 0.0f; s.z_clip_max = 16777215.0f;
        if (blend) { s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x000; }
        if (geo == 0)      quad(v, 8, 8, 248, 248, 0.1f, 0.2f, 0.6f, 0.7f, 1.0f);   /* magnified */
        else if (geo == 1) quad(v, 20, 30, 84, 94, -1.0f, -0.5f, 3.0f, 3.5f, 1.0f); /* minified, wraps */
        else               quad(v, 10, 10, 200, 150, 0.0f, 0.0f, 1.3f, 0.9f, 2.0f); /* projective */
        memset(target, 0x55, sizeof target); memset(depth, 0xff, sizeof depth);
        if (nv2a_metal_draw(&s, tex[f], tex_bytes[f], target, sizeof target,
                            depth, sizeof depth, (const float (*)[16][4])v, 6, 5) < 0) {
            fprintf(stderr, "case f=%d m=%d rep=%d geo=%d blend=%d rejected: %s\n",
                    f, m, rep, geo, blend, nv2a_metal_last_reject());
            ++rejected;
        }
        nv2a_metal_sync();
        if (dump) fwrite(target, 1, sizeof target, dump);
        ++cases;
    }
    if (dump) fclose(dump);
    nv2a_metal_report();   /* the [METAL] hw textures line proves which sampler ran */
    printf("cases=%u rejected=%u\n", cases, rejected);
    return rejected ? 1 : 0;
}
