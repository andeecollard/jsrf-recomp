/* G54: A LINE OF TWO INDICES AND A POINT OF ONE NOW PAINT PIXELS.
 *
 * The Metal path rejected POINTS, LINES, LINE_LOOP and LINE_STRIP and the CPU
 * fallback draws none of them, so a single segment or point was lost. They
 * are now screen-space quads through the triangle path (draw_points_lines).
 * Drawn here into a 96x64 565 target, counting the pixels that changed:
 *   - a 2-index LINE from (10,10) to (60,30): about its length in pixels;
 *   - a 1-index POINT at size 4 (SET_POINT_SIZE 32, in 1/8 px): 16 pixels;
 *   - a 1-index POINT at size 0: at least one pixel;
 *   - a 3-vertex LINE_LOOP paints more than the same LINE_STRIP (it closes);
 *   - both colours come from the vertices' diffuse.
 * CONTROL: the same two vertices as TRIANGLES paint nothing. A unit image
 * (PPM) is written when a path is given. Needs a GPU. */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define W 96u
#define H 64u
static uint8_t tgt[W * H * 2], dep[W * H * 4];

static void state(NV2ATextureCopy *s, float point_size)
{
    memset(s, 0, sizeof *s);
    s->clip_w = W; s->clip_h = H; s->target_pitch = W * 2; s->target_bpp = 2; s->depth_pitch = W * 4;
    s->z_clip_min = 0.0f; s->z_clip_max = 16777215.0f;
    s->untextured = 1; s->modulate = 1; s->combiner_count = 1;
    s->color_icw[0] = 0x04200000u; s->alpha_icw[0] = 0x14200000u;   /* R0 = V0 */
    s->color_ocw[0] = 0x000000C0u; s->alpha_ocw[0] = 0x000000C0u;
    s->cull_face = 0x405;                                              /* back-face culling on: must not touch points/lines */
    s->point_size = point_size;
}
static void vert(float v[16][4], float x, float y, float r, float g, float b)
{
    memset(v, 0, sizeof(float) * 16 * 4);
    v[0][0] = x; v[0][1] = y; v[0][2] = 0.25f * 16777215.0f; v[0][3] = 1.0f;
    v[3][0] = r; v[3][1] = g; v[3][2] = b; v[3][3] = 1.0f;
    for (int u = 0; u < 4; ++u) v[9 + u][3] = 1.0f;
}
static unsigned draw(const NV2ATextureCopy *s, float (*v)[16][4], unsigned n, unsigned prim, int *rc)
{
    unsigned changed = 0;
    memset(tgt, 0, sizeof tgt); memset(dep, 0, sizeof dep);
    nv2a_metal_invalidate(NULL);
    *rc = nv2a_metal_draw(s, NULL, 0, tgt, sizeof tgt, dep, sizeof dep, (const float (*)[16][4])v, n, prim);
    nv2a_metal_sync();
    for (unsigned i = 0; i < W * H; ++i) if (tgt[2 * i] || tgt[2 * i + 1]) ++changed;
    return changed;
}
static void ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i) {
        uint16_t c = (uint16_t)(tgt[2 * i] | tgt[2 * i + 1] << 8);
        uint8_t o[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
        fwrite(o, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static float v[8][16][4];
    NV2ATextureCopy s;
    unsigned px, px_strip, px_loop;
    int rc;
    state(&s, 1.0f);

    vert(v[0], 10.0f, 10.0f, 1, 0, 0); vert(v[1], 60.0f, 30.0f, 1, 0, 0);
    px = draw(&s, v, 2, 2, &rc);
    CHECK(rc == 1 && px >= 45 && px <= 80, "a 2-index LINE paints its segment (%u px, %d line)", px, rc);
    {   uint16_t c = (uint16_t)(tgt[2 * (20 * W + 35)] | tgt[2 * (20 * W + 35) + 1] << 8);
        CHECK((c >> 11) > 20 && ((c >> 5) & 63) < 8, "in the vertices' diffuse (red: %04X at 35,20)", c); }
    if (argc > 1) { char p[512]; snprintf(p, sizeof p, "%s-line.ppm", argv[1]); ppm(p); }

    state(&s, 4.0f);                                   /* SET_POINT_SIZE 32 */
    vert(v[0], 70.0f, 40.0f, 0, 1, 0);
    px = draw(&s, v, 1, 1, &rc);
    CHECK(rc == 1 && px == 16, "a 1-index POINT at size 4 paints 4x4 = %u px", px);
    if (argc > 1) { char p[512]; snprintf(p, sizeof p, "%s-point.ppm", argv[1]); ppm(p); }
    state(&s, 0.0f);
    px = draw(&s, v, 1, 1, &rc);
    CHECK(rc == 1 && px >= 1 && px <= 2, "a POINT at size 0 still paints (%u px)", px);

    state(&s, 1.0f);
    vert(v[0], 10.0f, 10.0f, 0, 0, 1); vert(v[1], 80.0f, 10.0f, 0, 0, 1); vert(v[2], 45.0f, 55.0f, 0, 0, 1);
    px_strip = draw(&s, v, 3, 4, &rc);
    px_loop = draw(&s, v, 3, 3, &rc);
    CHECK(px_strip > 100 && px_loop > px_strip + 40, "LINE_LOOP closes itself (its third segment is about 57 px): loop %u px, strip %u px", px_loop, px_strip);

    vert(v[0], 10.0f, 10.0f, 1, 0, 0); vert(v[1], 60.0f, 30.0f, 1, 0, 0);
    px = draw(&s, v, 2, 5, &rc);
    CHECK(px == 0, "CONTROL: the same two vertices as TRIANGLES paint nothing (%u px, rc %d)", px, rc);

    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
