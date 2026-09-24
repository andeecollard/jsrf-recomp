/* SZ_X1R5G5B5 (0x03) AND SZ_A4R4G4B4 (0x04) ON THE METAL SOFTWARE SAMPLER.
 *
 * The CPU sampler has decoded both since 21 Sep 2026 (nv2a_texture_copy.c:
 * sz16/argb4), but the shader's texel() only ever named RGBA8, DXT1 and
 * DXT3, so these fell through to the LINEAR R5G6B5 branch: row-major
 * addressing where the bytes are Morton-swizzled, 5:6:5 where they are 5:5:5
 * or 4:4:4:4, alpha forced to 1 where A4R4G4B4 has real alpha, and uv never
 * normalised, so a coordinate of 0..1 read a single texel. The player's
 * Rokkaku-dai session bound 0x04 ~207,000 times and 0x03 ~3,000.
 *
 * The CPU sampler is the reference, byte for byte, as in metal_copy_test.c.
 * Each case is built so the old shader cannot pass it by accident: every
 * texel differs, the triangle spans the texture in normalised coordinates,
 * and the A4R4G4B4 arms blend by source alpha so a forced-opaque alpha
 * changes the output. */
#include "nv2a_metal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)

/* Where the two differ, say how many pixels and the first, so a failure
 * names what went wrong instead of just "memcmp". */
static int same(const char *what, const uint8_t *cpu, const uint8_t *gpu, size_t n)
{
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i += 2)
        if (cpu[i] != gpu[i] || cpu[i+1] != gpu[i+1]) { if (!bad) first = i; ++bad; }
    if (bad)
        fprintf(stderr, "FAIL %s: %zu of %zu pixels differ; first at %zu: cpu %02x%02x gpu %02x%02x\n",
                what, bad, n / 2, first / 2, cpu[first+1], cpu[first], gpu[first+1], gpu[first]);
    else fprintf(stderr, "ok   %s\n", what);
    return bad == 0;
}

int main(void)
{
    /* 16x16 level 0 + 8x8 + 4x4 = 512 + 128 + 32 bytes: w*h*2 per level. */
    uint8_t tex[672], cpu[512], gpu[512];
    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73u + 11u);
    NV2ATextureCopy s = {0};
    s.width = s.height = s.clip_w = s.clip_h = 16;
    s.pitch = 32; s.target_pitch = 32; s.target_bpp = 2;
    s.levels = 1; s.texture_mask = 1; s.sz16 = 1; s.repeat = 1;
    /* Screen (0,0)-(32,0)-(0,32) covers all 16x16; texcoords 0..2 over it
     * span the texture once across the visible 16 pixels. */
    float v[3][16][4] = {0};
    /* Diffuse white and opaque: the blend arms modulate by it, so the
     * fragment's alpha is the texel's alpha and nothing else. */
    for (unsigned i = 0; i < 3; ++i) {
        v[i][0][3] = v[i][9][3] = 1;
        v[i][3][0] = v[i][3][1] = v[i][3][2] = v[i][3][3] = 1;   /* oD0 */
    }
    v[1][0][0] = v[2][0][1] = 32; v[1][9][0] = v[2][9][1] = 2;
    int failed = 0;

    for (unsigned argb4 = 0; argb4 < 2; ++argb4)
    for (unsigned blend = 0; blend < 2; ++blend)
    for (unsigned linear = 0; linear < 2; ++linear) {
        char what[96];
        snprintf(what, sizeof what, "%s %s %s", argb4 ? "A4R4G4B4" : "X1R5G5B5",
                 linear ? "bilinear" : "point", blend ? "src-alpha blend" : "copy");
        s.argb4 = argb4; s.linear = linear; s.levels = 1; s.min_filter = 1;
        s.blend = blend; s.modulate = blend; s.blend_src = 0x302; s.blend_dst = 0x303;
        nv2a_metal_invalidate(gpu); memset(cpu, 0xcc, sizeof cpu); memcpy(gpu, cpu, sizeof cpu);
        CHECK(nv2a_texture_copy_triangle(&s, tex, 512, cpu, sizeof cpu, v[0], v[1], v[2]));
        int r = nv2a_metal_draw(&s, tex, 512, gpu, sizeof gpu, NULL, 0, v, 3, 5);
        if (r != 1) fprintf(stderr, "Metal rejected %s: %s\n", what, nv2a_metal_last_reject());
        CHECK(r == 1);
        CHECK(nv2a_metal_sync());
        if (!same(what, cpu, gpu, sizeof cpu)) failed = 1;
    }

    /* The mip chain: texcoords 0..4 over 16 pixels is LOD 1, so this reads
     * level 1, which sits at w*h*2 = 512 bytes with pitch 8*2 -- not at the
     * level-0 pitch times h the linear formats use. Trilinear blends 1 and 2.
     * Through one pass-through combiner stage, because only the combiner path
     * of the CPU sampler walks the mip chain (as in metal_copy_test.c). */
    v[1][9][0] = v[2][9][1] = 4;
    s.combiner_count = 1; s.color_icw[0] = 0x08200000; s.alpha_icw[0] = 0x18200000;
    s.color_ocw[0] = s.alpha_ocw[0] = 0xc00; s.modulate = 0;
    for (unsigned argb4 = 0; argb4 < 2; ++argb4) {
        char what[96];
        snprintf(what, sizeof what, "%s mip chain", argb4 ? "A4R4G4B4" : "X1R5G5B5");
        s.argb4 = argb4; s.linear = 1; s.levels = 3; s.min_filter = 6; s.blend = 0;
        nv2a_metal_invalidate(gpu); memset(cpu, 0xcc, sizeof cpu); memcpy(gpu, cpu, sizeof cpu);
        CHECK(nv2a_texture_copy_triangle_depth(&s, tex, sizeof tex, cpu, sizeof cpu, NULL, 0, v[0], v[1], v[2]));
        int r = nv2a_metal_draw(&s, tex, sizeof tex, gpu, sizeof gpu, NULL, 0, v, 3, 5);
        if (r != 1) fprintf(stderr, "Metal rejected %s: %s\n", what, nv2a_metal_last_reject());
        CHECK(r == 1);
        CHECK(nv2a_metal_sync());
        if (!same(what, cpu, gpu, sizeof cpu)) failed = 1;
    }
    if (failed) return 1;
    puts("Metal GPU SZ_X1R5G5B5 and SZ_A4R4G4B4 (point, bilinear, blended, mipped) match CPU");
    return 0;
}
