/* G54 RECOMP_INVISIBLE_DRAW: THE FRAGMENT COUNT IS THE ONE THE EYE SEES.
 *
 * nv2a_metal_measure_arm puts the next nv2a_metal_draw's fragments that
 * pass every test into a Metal visibility slot; collect returns them. A
 * 48x32 quad (1,536 px) textured with an rgba8 texture:
 *   - opaque texels, alpha test GREATER 0: every pixel passes (1,536);
 *   - CONTROL, the case the instrument exists for: texels of alpha 0 under
 *     the same alpha test -- the draw rasterises and paints nothing, and the
 *     count says 0;
 *   - a draw with no slot armed is not counted, and two armed draws in one
 *     window keep separate slots.
 * Plus nv2a_clipped_triangle_area, the area the executor compares against:
 * inside, half outside, and behind the camera. Needs a GPU. */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include "nv2a_drop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define W 96u
#define H 64u
#define TW 8u
static uint8_t tgt[W * H * 2], dep[W * H * 4], tex[TW * TW * 4];

static void state(NV2ATextureCopy *s)
{
    memset(s, 0, sizeof *s);
    s->clip_w = W; s->clip_h = H; s->target_pitch = W * 2; s->target_bpp = 2; s->depth_pitch = W * 4;
    s->z_clip_min = 0.0f; s->z_clip_max = 16777215.0f;
    s->width = s->height = TW; s->pitch = TW * 4; s->levels = 1; s->rgba8 = 1; s->min_filter = 1; s->repeat = 1;
    s->texture_mask = 1; s->modulate = 1; s->combiner_count = 1;
    s->color_icw[0] = 0x08200000u; s->alpha_icw[0] = 0x18200000u;     /* R0 = T0, alpha T0.a */
    s->color_ocw[0] = 0x000000C0u; s->alpha_ocw[0] = 0x000000C0u;
    s->alpha_test = 1; s->alpha_ref = 0;                                /* GREATER 0, the gate's one function */
}
static void quad(float v[6][16][4], float x0, float y0, float x1, float y1)
{
    const float P[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    memset(v, 0, sizeof(float) * 6 * 16 * 4);
    for (int j = 0; j < 6; ++j) {
        v[j][0][0] = x0 + (x1 - x0) * P[j][0]; v[j][0][1] = y0 + (y1 - y0) * P[j][1];
        v[j][0][2] = 0.25f * 16777215.0f; v[j][0][3] = 1.0f;
        v[j][3][0] = v[j][3][1] = v[j][3][2] = v[j][3][3] = 1.0f;
        v[j][9][0] = P[j][0]; v[j][9][1] = P[j][1]; v[j][9][3] = 1.0f;
    }
}
static int draw(NV2ATextureCopy *s, float v[6][16][4], int slot)
{
    if (slot >= 0 && !nv2a_metal_measure_arm((unsigned)slot)) return -9;
    int r = nv2a_metal_draw(s, tex, sizeof tex, tgt, sizeof tgt, dep, sizeof dep, (const float (*)[16][4])v, 6, 5);
    nv2a_metal_measure_disarm();
    return r;
}

int main(void)
{
    static float v[6][16][4];
    static unsigned long long counts[8]; static uint8_t used[8];
    NV2ATextureCopy s;
    memset(tgt, 0, sizeof tgt); memset(dep, 0, sizeof dep);
    nv2a_metal_invalidate(NULL);
    state(&s);

    for (unsigned i = 0; i < TW * TW; ++i) { tex[4*i] = 0x40; tex[4*i+1] = 0x80; tex[4*i+2] = 0xC0; tex[4*i+3] = 0xFF; }
    quad(v, 8, 8, 56, 40);
    CHECK(draw(&s, v, 0) >= 0, "opaque draw submitted");
    nv2a_metal_measure_collect(counts, used, 4);
    CHECK(used[0] && counts[0] == 48u * 32u, "opaque texels under alpha test: %llu of 1536 fragments pass", counts[0]);

    for (unsigned i = 0; i < TW * TW; ++i) tex[4*i+3] = 0;
    nv2a_metal_invalidate(NULL);
    CHECK(draw(&s, v, 1) >= 0, "alpha-0 draw submitted");
    CHECK(draw(&s, v, -1) >= 0, "an unarmed draw submitted");
    nv2a_metal_measure_collect(counts, used, 4);
    CHECK(used[1] && counts[1] == 0, "CONTROL: alpha-0 texels under alpha test rasterise and pass nothing (%llu)", counts[1]);
    CHECK(!used[0] && !used[2], "the unarmed draw took no slot, and the last window's slot is cleared");

    for (unsigned i = 0; i < TW * TW; ++i) tex[4*i+3] = 0xFF;
    nv2a_metal_invalidate(NULL);
    quad(v, 0, 0, 20, 20); draw(&s, v, 2);
    quad(v, 60, 30, 90, 60); draw(&s, v, 3);
    nv2a_metal_measure_collect(counts, used, 4);
    CHECK(counts[2] == 400 && counts[3] == 900, "two slots in one window stay apart (%llu, %llu)", counts[2], counts[3]);

    /* ROKKAKU'S BUILDINGS: the fogged general final combiner (CW0 130E0300,
     * CW1 1C80), LINEAR fog (2, -0.00025) at coordinate 1000 (f 0.75),
     * opaque texture, diffuse alpha 1, alpha test GREATER 0. Every fragment
     * must pass, specialised (the default) and generic
     * (RECOMP_METAL_SPECIALISE_COMBINERS=0, the jsrf_metal_invisible_generic
     * arm) alike -- G = R0.a = 1. CONTROL: the same draw with G reading the
     * ZERO register (CW1 001C80 -> 00000080 | G 0) passes nothing. */
    {   NV2ATextureCopy g;
        state(&g);
        g.color_icw[0] = 0x08040000u; g.alpha_icw[0] = 0x18140000u;      /* R0 = T0 * V0, alpha T0.a * V0.a */
        g.final_general = 1; g.final_cw0 = 0x130E0300u; g.final_cw1 = 0x1C80u;
        g.fog_enable = 1; g.fog_mode = 0x2601u; g.fog_p0 = 2.0f; g.fog_p1 = -0.00025f; g.fog_color = 0x005E77A5u;
        quad(v, 8, 8, 56, 40);
        for (int j = 0; j < 6; ++j) v[j][5][0] = 1000.0f;
        nv2a_metal_invalidate(NULL);
        draw(&g, v, 4);
        g.final_cw1 = 0x0080u;
        draw(&g, v, 5);
        nv2a_metal_measure_collect(counts, used, 8);
        printf("  fogged general final combiner under alpha test (%s): %llu of 1536 pass; with G = ZERO %llu\n",
               getenv("RECOMP_METAL_SPECIALISE_COMBINERS") ? "generic" : "specialised", counts[4], counts[5]);
        CHECK(used[4] && counts[4] == 48u * 32u, "Rokkaku's building state (130E0300/1C80, fog, alpha test GREATER 0) passes every fragment (%llu)", counts[4]);
        CHECK(used[5] && counts[5] == 0, "CONTROL: with G reading ZERO the alpha test discards it all (%llu)", counts[5]); }

    /* THE CAUSE (G54): X8R8G8B8's padding byte is not alpha. Rokkaku's
     * buildings multiply by an X8R8G8B8 lightmap, alpha included; its padding
     * is 0. As X8R8G8B8 (xrgb8) every fragment must pass the alpha test --
     * through the hardware sampler (the _hwtex arm) and the shader's own
     * sampler alike. CONTROL: the same bytes declared A8R8G8B8 pass none. */
    {   NV2ATextureCopy x;
        for (unsigned i = 0; i < TW * TW; ++i) { tex[4*i] = 0x30; tex[4*i+1] = 0x60; tex[4*i+2] = 0x90; tex[4*i+3] = 0x00; }
        state(&x); x.xrgb8 = 1;
        quad(v, 8, 8, 56, 40);
        nv2a_metal_invalidate(NULL);
        draw(&x, v, 6);
        nv2a_metal_measure_collect(counts, used, 8);
        CHECK(used[6] && counts[6] == 48u * 32u, "X8R8G8B8 with a zero padding byte is opaque: %llu of 1536 fragments pass the alpha test", counts[6]);
        x.xrgb8 = 0;
        nv2a_metal_invalidate(NULL);
        draw(&x, v, 7);
        nv2a_metal_measure_collect(counts, used, 8);
        CHECK(used[7] && counts[7] == 0, "CONTROL: the same bytes as A8R8G8B8 have alpha 0 and pass none (%llu)", counts[7]); }

    {   const float a[4] = { 10, 10, 0, 1 }, b[4] = { 50, 10, 0, 1 }, c[4] = { 10, 30, 0, 1 };
        const float d[4] = { -40, 10, 0, 1 }, e[4] = { 40, 10, 0, 1 }, f[4] = { -40, 30, 0, 1 };
        const float g[4] = { 10, 10, 0, -1 };
        CHECK(fabsf(nv2a_clipped_triangle_area(a, b, c, W, H) - 400.0f) < 0.01f, "area inside the surface: 400");
        CHECK(fabsf(nv2a_clipped_triangle_area(d, e, f, W, H) - 200.0f) < 0.01f,
              "area clipped at x = 0: 200 of the 800 triangle (%g)", nv2a_clipped_triangle_area(d, e, f, W, H));
        CHECK(nv2a_clipped_triangle_area(g, b, c, W, H) == 0.0f, "a vertex behind the camera: no area claimed"); }

    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
