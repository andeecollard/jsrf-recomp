/* G27b: does moving the depth test in front of the shader change the image?
 *
 * One scene, drawn with RECOMP_METAL_EARLY_Z off and on; metal_earlyz_check.sh
 * requires the two targets to be BYTE-IDENTICAL, because every early variant
 * claims to be exact. A third arm with RECOMP_METAL_EARLY_Z_REF0=1 -- a known
 * approximation -- must DIFFER, or the scene cannot see a wrong early test.
 *
 * The scene is built so that a discard that ran after an early depth WRITE
 * would show: a far opaque floor; an alpha-tested checker texture (half its
 * texels alpha 0) drawn with depth writes OFF (the fs_hw_early_nw case);
 * the same checker drawn with depth writes ON (must stay late); then an
 * opaque quad BEHIND the depth-writing checker, which shows through its
 * transparent texels only if they wrote no depth; and a z-culled draw with
 * no writes. Needs a real GPU: built, not registered.
 */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256u
#define H 256u
#define TW 16u
static uint8_t target[W*H*2], depth[W*H*4];
static uint8_t checker[TW*TW*4], solid[TW*TW*4];

static unsigned morton(unsigned x, unsigned y)
{ unsigned i = 0, bit = 0; for (unsigned b = 1; b < TW; b <<= 1) { if (x & b) i |= 1u << bit; ++bit; if (y & b) i |= 1u << bit; ++bit; } return i; }

static void quad(float v[6][16][4], float x0, float y0, float x1, float y1, float z)
{
    const float P[6][4] = {{x0,y0,0,0},{x1,y0,1,0},{x1,y1,1,1},{x0,y0,0,0},{x1,y1,1,1},{x0,y1,0,1}};
    memset(v, 0, sizeof(float) * 6 * 16 * 4);
    for (int j = 0; j < 6; ++j) {
        v[j][0][0] = P[j][0]; v[j][0][1] = P[j][1]; v[j][0][2] = z * 16777215.0f; v[j][0][3] = 1.0f;
        v[j][3][0] = v[j][3][1] = v[j][3][2] = v[j][3][3] = 1.0f;
        v[j][9][0] = P[j][2] * 3.0f; v[j][9][1] = P[j][3] * 3.0f; v[j][9][3] = 1.0f;
    }
}

static int draw(const uint8_t *tex, float x0, float y0, float x1, float y1, float z,
                int alpha_test, int depth_write, int z_cull)
{
    NV2ATextureCopy s; float v[6][16][4];
    memset(&s, 0, sizeof s);
    s.width = s.height = TW; s.pitch = TW*4; s.levels = 1; s.rgba8 = 1; s.texture_mask = 1;
    s.min_filter = 1; s.linear = 0; s.repeat = 1; s.modulate = 1;
    s.clip_w = W; s.clip_h = H; s.target_pitch = W*2; s.target_bpp = 2; s.depth_pitch = W*4;
    s.depth_test = 1; s.depth_write = (unsigned)depth_write; s.depth_func = 0x203;   /* LEQUAL */
    s.alpha_test = (unsigned)alpha_test; s.alpha_ref = 0;
    s.z_clip_min = 0.0f; s.z_clip_max = 16777215.0f;
    if (z_cull) { s.z_cull = 1; s.z_clip_min = 0.3f * 16777215.0f; s.z_clip_max = 0.6f * 16777215.0f; }
    quad(v, x0, y0, x1, y1, z);
    if (nv2a_metal_draw(&s, tex, sizeof checker, target, sizeof target, depth, sizeof depth,
                        (const float (*)[16][4])v, 6, 5) < 0) {
        fprintf(stderr, "draw rejected: %s\n", nv2a_metal_last_reject());
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    FILE *dump = argc > 1 ? fopen(argv[1], "wb") : NULL;
    int ok = 1;
    if (argc > 1 && !dump) { perror(argv[1]); return 1; }
    for (unsigned y = 0; y < TW; ++y)
        for (unsigned x = 0; x < TW; ++x) {
            uint8_t *c = checker + 4 * morton(x, y), *d = solid + 4 * morton(x, y);
            int on = ((x / 4) + (y / 4)) & 1;
            c[0] = 40; c[1] = 200; c[2] = 240; c[3] = on ? 255 : 0;              /* BGRA */
            d[0] = (uint8_t)(x * 16); d[1] = 60; d[2] = (uint8_t)(y * 16); d[3] = 255;
        }
    memset(target, 0x33, sizeof target);
    for (size_t i = 0; i < sizeof depth; i += 4) { depth[i] = 0; depth[i+1] = depth[i+2] = depth[i+3] = 0xff; }
    nv2a_metal_invalidate(NULL);

    ok &= draw(solid,   0,   0, 256, 256, 0.90f, 0, 1, 0);   /* far opaque floor, writes depth */
    ok &= draw(checker, 20,  20, 140, 140, 0.50f, 1, 0, 0);   /* alpha test, NO depth write: early_nw */
    ok &= draw(checker, 100, 100, 230, 230, 0.40f, 1, 1, 0);   /* alpha test WITH depth write: late */
    ok &= draw(solid,   90,  90, 240, 240, 0.45f, 0, 1, 0);   /* behind it: shows through alpha-0 texels */
    ok &= draw(checker, 10, 150, 120, 250, 0.70f, 1, 0, 0);   /* no write, partly behind the floor? no: in front */
    ok &= draw(solid,   30, 160,  90, 240, 0.95f, 0, 0, 0);   /* behind the floor: must be culled either way */
    ok &= draw(solid,  150,  10, 250,  90, 0.20f, 0, 0, 1);   /* z-culled (outside 0.3..0.6), no writes */
    ok &= draw(checker,160,  20, 240,  80, 0.50f, 1, 0, 1);   /* z range kept, alpha test, no writes */
    nv2a_metal_sync();
    if (dump) { fwrite(target, 1, sizeof target, dump); fclose(dump); }
    nv2a_metal_report();
    return ok ? 0 : 1;
}
