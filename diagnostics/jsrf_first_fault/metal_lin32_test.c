/* LU_IMAGE_A8R8G8B8 (0x12) AND LU_IMAGE_X8R8G8B8 (0x1E): THE GRAFFITI CANVAS.
 *
 * Roboy's graffiti studio could not paint. Its canvas, brush preview and
 * palette are pitch-linear A8R8G8B8 textures (format register 0x00011229)
 * that the game fills by CPU writes -- BGRA bytes at y*pitch + x*4 -- and
 * draws as a full quad into the linear R5G6B5 back buffer. The gate accepted
 * only 0x11 among the linear formats, so every such draw was refused as
 * "texture format / mip layout": 894 of 14,911 batches in the player's
 * editor session.
 *
 * Three layers, each of which the old code fails:
 *   1. THE GATE. The measured JSRF copy program with its format byte changed
 *      to 0x12 / 0x1E must prepare, as a one-level image rectangle whose
 *      pitch is at least four bytes a texel. A format this change did not
 *      add (0x13 Y8) must still be refused.
 *   2. THE CPU SAMPLER, against bytes written out by hand here rather than
 *      by texel(): a 1:1 copy of the canvas into a 565 target. This is the
 *      exact shape of the row-copy fast path, which assumed 565 bytes and
 *      would memcpy BGRA into the target if it let lin32 through.
 *   3. THE METAL SOFTWARE SAMPLER against the CPU sampler, as
 *      metal_sz16_test.c does (to one 565 step, see same()): point and
 *      bilinear, copy and src-alpha blend,
 *      A8 and X8. Coordinates are TEXELS, unnormalised, as for 0x11 -- the
 *      triangle spans 0..32 in both screen and texture space -- and the pitch
 *      is padded past width*4 so row addressing has to use it. */
#include "nv2a_metal.h"
#include "texture_copy_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)

enum { W = 16, H = 16, PITCH = 80, TPITCH = W * 2 };

/* `slack` is how many 565 steps a channel may differ by. 0 is byte for
 * byte. 1 is for 8-bit sources only: the GPU and CPU samplers already round
 * a handful of 8-bit channels to 565 one step apart on the SWIZZLED
 * A8R8G8B8 path this change does not touch (measured 24 Sep 2026: the same
 * 4 of 256 pixels, green ed4c against ed2c, with rgba8 = 1). A wrong
 * address, byte order or alpha is many steps out, so 1 still catches it. */
static int same(const char *what, const uint8_t *cpu, const uint8_t *gpu, size_t n, int slack)
{
    size_t bad = 0, near = 0, first = 0;
    for (size_t i = 0; i < n; i += 2) {
        unsigned a = cpu[i] | (unsigned)cpu[i+1] << 8, b = gpu[i] | (unsigned)gpu[i+1] << 8;
        if (a == b) continue;
        int dr = (int)(a >> 11) - (int)(b >> 11), dg = (int)((a >> 5) & 63) - (int)((b >> 5) & 63),
            db = (int)(a & 31) - (int)(b & 31);
        if (abs(dr) <= slack && abs(dg) <= slack && abs(db) <= slack) { ++near; continue; }
        if (!bad) first = i;
        ++bad;
    }
    if (bad)
        fprintf(stderr, "FAIL %s: %zu of %zu pixels differ; first at %zu: want %02x%02x got %02x%02x\n",
                what, bad, n / 2, first / 2, cpu[first+1], cpu[first], gpu[first+1], gpu[first]);
    else if (near) fprintf(stderr, "ok   %s (%zu pixels one 565 step apart)\n", what, near);
    else fprintf(stderr, "ok   %s\n", what);
    return bad == 0;
}

int main(void)
{
    static uint8_t tex[PITCH * H], cpu[TPITCH * H], gpu[TPITCH * H], want[TPITCH * H];
    uint32_t m[2048];
    int failed = 0;

    /* ---- 1. the gate ------------------------------------------------- */
    {
        static const struct { uint32_t fmt; unsigned pitch; unsigned lin32; const char *what; } g[] = {
            { 0x00011229u, PITCH, 1, "0x12 A8R8G8B8 prepares" },
            { 0x00011E29u, PITCH, 2, "0x1E X8R8G8B8 prepares" },
            { 0x00011229u, W * 4, 1, "0x12 at exactly width*4 prepares" },
            { 0x00011229u, W * 2, 0, "0x12 with a 16-bit pitch is refused" },
            { 0x00011329u, PITCH, 0, "0x13 Y8 is still refused" },
        };
        for (unsigned i = 0; i < sizeof g / sizeof g[0]; ++i) {
            NV2ATextureCopy s; memset(&s, 0, sizeof s);
            copy_methods(m, W, H, g[i].pitch, TPITCH, 2);
            m[0x1b04/4] = g[i].fmt;
            const char *err = nv2a_texture_copy_prepare(m, &s);
            int ok = g[i].lin32 ? (!err && s.lin32 == g[i].lin32 && s.width == W && s.height == H
                                   && s.pitch == g[i].pitch && s.levels == 1 && !s.repeat)
                                : err != NULL;
            fprintf(stderr, "%s %s%s%s\n", ok ? "ok  " : "FAIL", g[i].what,
                    err ? " -- " : "", err ? err : "");
            if (!ok) failed = 1;
        }
    }

    /* ---- 2. the canvas copy, against a hand-written expectation ------ */
    /* Every channel 0 or 255 so 565 holds it exactly, and neighbours differ
     * in at least one channel, so a wrong stride or byte order shows. The
     * pitch padding is poison: reading it would give magenta. */
    memset(tex, 0, sizeof tex);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < PITCH / 4; ++x) {
        uint8_t *p = tex + y * PITCH + x * 4;
        unsigned c = (x * 5u + y * 3u) % 7u + 1u;           /* 1..7: never black */
        if (x >= W) { p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255; continue; }
        p[0] = c & 1 ? 255 : 0; p[1] = c & 2 ? 255 : 0; p[2] = c & 4 ? 255 : 0; p[3] = 255;
    }
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const uint8_t *p = tex + y * PITCH + x * 4;
        uint16_t v = (uint16_t)((p[2] ? 0xf800u : 0) | (p[1] ? 0x07e0u : 0) | (p[0] ? 0x001fu : 0));
        want[(y * W + x) * 2] = (uint8_t)v; want[(y * W + x) * 2 + 1] = (uint8_t)(v >> 8);
    }
    /* Screen (0,0)-(32,0)-(0,32) covers the 16x16 target; texcoords equal to
     * the position are a 1:1 texel copy, which is what the row-copy path
     * looks for. */
    float v[3][16][4] = {{{0}}};
    for (unsigned i = 0; i < 3; ++i) {
        v[i][0][3] = v[i][9][3] = 1;
        v[i][3][0] = v[i][3][1] = v[i][3][2] = v[i][3][3] = 1;   /* oD0 white */
    }
    v[1][0][0] = v[2][0][1] = 32; v[1][9][0] = v[2][9][1] = 32;
    for (unsigned lin = 1; lin <= 2; ++lin) {
        NV2ATextureCopy s; memset(&s, 0, sizeof s);
        copy_methods(m, W, H, PITCH, TPITCH, 2);
        m[0x1b04/4] = lin == 1 ? 0x00011229u : 0x00011E29u;
        const char *err = nv2a_texture_copy_prepare(m, &s);
        if (err) { fprintf(stderr, "FAIL canvas copy: gate refused: %s\n", err); failed = 1; continue; }
        memset(cpu, 0xcc, sizeof cpu); memcpy(gpu, cpu, sizeof gpu);
        CHECK(nv2a_texture_copy_triangle(&s, tex, sizeof tex, cpu, sizeof cpu, v[0], v[1], v[2]));
        if (!same(lin == 1 ? "CPU canvas copy 0x12 vs hand-decoded" : "CPU canvas copy 0x1E vs hand-decoded",
                  want, cpu, sizeof cpu, 0)) failed = 1;
        nv2a_metal_invalidate(gpu);
        int r = nv2a_metal_draw(&s, tex, sizeof tex, gpu, sizeof gpu, NULL, 0, v, 3, 5);
        if (r != 1) fprintf(stderr, "Metal rejected the canvas copy: %s\n", nv2a_metal_last_reject());
        CHECK(r == 1);
        CHECK(nv2a_metal_sync());
        if (!same(lin == 1 ? "GPU canvas copy 0x12 vs hand-decoded" : "GPU canvas copy 0x1E vs hand-decoded",
                  want, gpu, sizeof gpu, 0)) failed = 1;
    }

    /* ---- 3. GPU vs CPU ------------------------------------------------ */
    /* Now every byte differs, including alpha, so bilinear weights, alpha
     * blending and the X8 padding all reach the output. */
    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73u + 11u);
    NV2ATextureCopy s; memset(&s, 0, sizeof s);
    s.width = W; s.height = H; s.clip_w = s.clip_h = W;
    s.pitch = PITCH; s.target_pitch = TPITCH; s.target_bpp = 2;
    s.levels = 1; s.texture_mask = 1;
    /* A quarter-texel offset so bilinear actually blends four texels, and a
     * coordinate a 0..1 reading could never reach. */
    for (unsigned i = 0; i < 3; ++i) { v[i][9][0] += .25f; v[i][9][1] += .25f; }
    for (unsigned lin = 1; lin <= 2; ++lin)
    for (unsigned blend = 0; blend < 2; ++blend)
    for (unsigned linear = 0; linear < 2; ++linear) {
        char what[96];
        snprintf(what, sizeof what, "%s %s %s", lin == 1 ? "LIN A8R8G8B8" : "LIN X8R8G8B8",
                 linear ? "bilinear" : "point", blend ? "src-alpha blend" : "copy");
        /* Minification and magnification filters agree. They have to: the
         * shader's LOD scales uv by the texture size, which for a texel
         * coordinate is a factor of W too many, so a 1:1 image rectangle
         * reads as minified and takes the MIN filter, while the CPU's
         * uncombined path always takes the mag filter. That is true of 0x11
         * already and is not what this test is about. */
        s.lin32 = lin; s.linear = linear; s.min_filter = linear ? 2 : 1;
        s.blend = blend; s.modulate = blend; s.blend_src = 0x302; s.blend_dst = 0x303;
        nv2a_metal_invalidate(gpu); memset(cpu, 0xcc, sizeof cpu); memcpy(gpu, cpu, sizeof cpu);
        CHECK(nv2a_texture_copy_triangle(&s, tex, sizeof tex, cpu, sizeof cpu, v[0], v[1], v[2]));
        int r = nv2a_metal_draw(&s, tex, sizeof tex, gpu, sizeof gpu, NULL, 0, v, 3, 5);
        if (r != 1) fprintf(stderr, "Metal rejected %s: %s\n", what, nv2a_metal_last_reject());
        CHECK(r == 1);
        CHECK(nv2a_metal_sync());
        if (!same(what, cpu, gpu, sizeof cpu, 1)) failed = 1;
    }
    if (failed) return 1;
    puts("LU_IMAGE_A8R8G8B8 / X8R8G8B8: gate, CPU canvas copy and Metal (point, bilinear, blended) all agree");
    return 0;
}
