/* BUMPENVMAP (texture shader modes 6 and 7), CPU AND METAL, AGAINST A HAND
 * COMPUTATION.
 *
 * Rokkaku-dai's water is unit 0 = a du/dv map (SZ_X8R8G8B8, tiled), unit 1 =
 * mode 6 over a reflection map, combined by stage 0 into R0. Until
 * 24 Sep 2026 mode 6 was drawn as a plain 2D fetch of unit 1
 * (RECOMP_TEXMODE_APPROX) -- no displacement at all.
 *
 * The state is built through nv2a_texture_copy_prepare from METHOD WORDS, so
 * the gate's reading of NV097_SET_TEXTURE_SET_BUMP_ENV_MAT is under test too:
 * the words arrive as M00, M01, M11, M10 (xemu pgraph.c; the XDK's
 * D3DTSS numbering 22..25), and the matrix is deliberately asymmetric so a
 * transposed or unswizzled reading lands on different texels.
 *
 * The expectation is integer texel arithmetic, not a call into the code under
 * test: every du, dv is -1, 0 or +1 (bytes 0x81, 0x00, 0x7F under xemu's
 * sign3) and every matrix entry is a whole number of 1/16 texels of the
 * 16x16 environment map, so pixel (x,y) must show env texel
 *     (x + 2 du - 1 dv,  y + 1 du + 3 dv)   mod 16
 * sampled at a texel centre, far from any rounding edge.
 *
 * THE NEGATIVE CONTROL is the same draw with the displacement removed --
 * what RECOMP_TEXMODE_APPROX draws -- which must NOT match. The ctest arm
 * jsrf_metal_bump_approx runs the whole program with RECOMP_TEXMODE_BUMP=0
 * RECOMP_TEXMODE_APPROX=1: the gate approximates, and that arm passes only if
 * the output names the approximated state AND the hand expectation fails on
 * both sinks (PASS_REGULAR_EXPRESSION, so an unrelated early failure does not
 * count as the control firing). */
#include "nv2a_metal.h"
#include "texture_copy_state.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
#define N 16

static unsigned morton(unsigned x, unsigned y)
{
    unsigned i = 0, bit = 0;
    for (unsigned b = 1; b < 16; b <<= 1) { if (x & b) i |= 1u << bit; ++bit; if (y & b) i |= 1u << bit; ++bit; }
    return i;
}
/* The target's quantisation, as nv2a_texture_copy.c does it. The expectation
 * must sit well clear of a rounding edge, or a 1-LSB difference between the
 * CPU's float arithmetic and Metal's fast-math contraction would be read as a
 * bump defect: `margin` records the closest any expected channel came. */
static float margin = 1;
static unsigned q(float v, unsigned max)
{
    float x = fminf(1, fmaxf(0, v)) * max + .5f, f = x - floorf(x);
    if (v > 0 && v < 1) margin = fminf(margin, fminf(f, 1 - f));
    return (unsigned)x;
}
static uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* du/dv map, 4x4, X8R8G8B8: B = du, G = dv, R = luminance. */
static const int du_of[4][4] = {{ 1, 0,-1, 0},{ 0, 1, 1,-1},{-1,-1, 0, 1},{ 1, 0, 0,-1}};
static const int dv_of[4][4] = {{ 0, 1, 0,-1},{ 1,-1, 1, 0},{ 0, 1,-1,-1},{-1, 0, 1, 1}};
static uint8_t sbyte(int d) { return d > 0 ? 0x7f : d < 0 ? 0x81 : 0x00; }
/* Environment texel colour, A8R8G8B8 bytes B,G,R,A. */
static uint8_t env_r(unsigned x) { return (uint8_t)(x * 11 + 5); }
static uint8_t env_g(unsigned y) { return (uint8_t)(y * 11 + 5); }

static int compare(const char *what, const uint8_t *got, const uint16_t *want, int expect_match)
{
    unsigned bad = 0, first = 0;
    for (unsigned i = 0; i < N * N; ++i)
        if ((got[2*i] | got[2*i+1] << 8) != want[i]) { if (!bad) first = i; ++bad; }
    if (expect_match && bad)
        fprintf(stderr, "FAIL %s: %u of %u pixels differ from the hand expectation; first (%u,%u): got %04x want %04x\n",
                what, bad, N * N, first % N, first / N, got[2*first] | got[2*first+1] << 8, want[first]);
    else if (!expect_match && !bad)
        fprintf(stderr, "FAIL %s: the negative control matched the expectation -- the test has no teeth\n", what);
    else fprintf(stderr, "ok   %s (%u of %u pixels differ from the bumped expectation)\n", what, bad, N * N);
    return expect_match ? bad == 0 : bad != 0;
}

int main(void)
{
    uint8_t dudv[4 * 4 * 4], env[N * N * 4];
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
        uint8_t *p = dudv + 4 * morton(x, y);
        p[0] = sbyte(du_of[y][x]); p[1] = sbyte(dv_of[y][x]);
        p[2] = (x + y) & 1 ? 0xff : 0x00;   /* L: 1 or 0 exactly */
        p[3] = 0x5a;                          /* X8: undefined, must not matter */
    }
    for (unsigned y = 0; y < N; ++y) for (unsigned x = 0; x < N; ++x) {
        uint8_t *p = env + 4 * morton(x, y);
        p[0] = 0x80; p[1] = env_g(y); p[2] = env_r(x); p[3] = 0xff;
    }

    const float mat[4] = { 2 / 16.0f, 1 / 16.0f, -1 / 16.0f, 3 / 16.0f };   /* M00 M01 M10 M11 */
    const float lscale = 0.5f, loffset = 0.1875f;
    int failed = 0;
    for (unsigned mode = 6; mode <= 7; ++mode) {
        uint32_t m[2048];
        NV2ATextureCopy s, extra[3] = {{0}};
        copy_methods(m, N, N, 0, N * 2, 2);
        m[0x1b04/4] = 0x02210729;        /* unit 0: SZ_X8R8G8B8 4x4, 1 level */
        m[0x1b08/4] = 0x10101;           /* wrap */
        m[0x1b14/4] = 0x01012000;        /* point */
        m[0x1b44/4] = 0x04410629;        /* unit 1: SZ_A8R8G8B8 16x16, 1 level */
        m[0x1b48/4] = 0x10101; m[0x1b4c/4] = 0x4003ffc0; m[0x1b54/4] = 0x01012000;
        m[0x1e70/4] = 1 | mode << 5;
        /* NV097_SET_TEXTURE_SET_BUMP_ENV_MAT(1): M00, M01, M11, M10. */
        m[0x1b68/4] = f_bits(mat[0]); m[0x1b6c/4] = f_bits(mat[1]);
        m[0x1b70/4] = f_bits(mat[3]); m[0x1b74/4] = f_bits(mat[2]);
        m[0x1b78/4] = f_bits(lscale); m[0x1b7c/4] = f_bits(loffset);
        /* Stage 0: R0 = T1 * 1, colour and alpha, AB -> R0 (the water's OCW). */
        m[0x1e60/4] = 1; m[0xac0/4] = 0x09200000; m[0x260/4] = 0x19200000;
        m[0x1e40/4] = m[0xaa0/4] = 0xc0;
        const char *why = nv2a_texture_copy_prepare(m, &s);
        if (why) fprintf(stderr, "prepare refused mode %u: %s\n", mode, why);
        CHECK(!why);
        CHECK(!nv2a_texture_copy_prepare_image(m, 1, &extra[0]));
        CHECK(s.texture_mask == 3 && s.xrgb8 && extra[0].rgba8 && !extra[0].xrgb8);
        fprintf(stderr, "mode %u: gate says bump[1]=%u bump_approx=%u\n", mode, s.bump[1], s.bump_approx);
        if (s.bump[1]) {
            CHECK(s.bump[1] == mode && s.bump_input[1] == 0 && !s.bump_approx);
            CHECK(s.bump_mat[1][0] == mat[0] && s.bump_mat[1][1] == mat[1]
                  && s.bump_mat[1][2] == mat[2] && s.bump_mat[1][3] == mat[3]);
            CHECK(s.bump_scale[1] == lscale && s.bump_offset[1] == loffset);
        }
        s.extra_stages = extra; s.extra_texture[0] = env; s.extra_size[0] = sizeof env;

        /* (0,0)-(32,0)-(0,32): covers the 16x16 target; both coordinate
         * sets run 0..1 across it, so pixel x samples du/dv texel x/4 and,
         * before the displacement, env texel x. */
        float v[3][16][4] = {{{0}}};
        for (unsigned i = 0; i < 3; ++i) {
            v[i][0][3] = v[i][9][3] = v[i][10][3] = 1;
            for (unsigned k = 0; k < 4; ++k) v[i][3][k] = v[i][4][k] = 1;
        }
        v[1][0][0] = v[2][0][1] = 32;
        v[1][9][0] = v[2][9][1] = v[1][10][0] = v[2][10][1] = 2;

        uint16_t want[N * N];
        for (unsigned y = 0; y < N; ++y) for (unsigned x = 0; x < N; ++x) {
            int du = du_of[y / 4][x / 4], dv = dv_of[y / 4][x / 4];
            unsigned ex = (unsigned)((int)x + 2 * du - 1 * dv + 32) % N;
            unsigned ey = (unsigned)((int)y + 1 * du + 3 * dv + 32) % N;
            float k = mode == 7 ? lscale * (((x / 4 + y / 4) & 1) ? 1.0f : 0.0f) + loffset : 1.0f;
            float r = env_r(ex) / 255.0f * k, g = env_g(ey) / 255.0f * k, b = 0x80 / 255.0f * k;
            want[y * N + x] = (uint16_t)(q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31));
        }
        CHECK(margin > 0.02f);

        uint8_t cpu[N * N * 2], gpu[N * N * 2];
        char what[96];
        snprintf(what, sizeof what, "mode %u CPU", mode);
        memset(cpu, 0xcc, sizeof cpu);
        CHECK(nv2a_texture_copy_triangle_depth(&s, dudv, sizeof dudv, cpu, sizeof cpu, NULL, 0, v[0], v[1], v[2]));
        if (!compare(what, cpu, want, 1)) failed = 1;

        snprintf(what, sizeof what, "mode %u Metal", mode);
        nv2a_metal_invalidate(gpu); memset(gpu, 0xcc, sizeof gpu);
        int r = nv2a_metal_draw(&s, dudv, sizeof dudv, gpu, sizeof gpu, NULL, 0, v, 3, 5);
        if (r != 1) fprintf(stderr, "Metal rejected %s: %s\n", what, nv2a_metal_last_reject());
        CHECK(r == 1);
        CHECK(nv2a_metal_sync());
        if (!compare(what, gpu, want, 1)) failed = 1;

        /* THE NEGATIVE CONTROL, in process: the approximation's draw. */
        if (s.bump[1]) {
            NV2ATextureCopy flat = s;
            flat.bump[1] = 0;
            snprintf(what, sizeof what, "mode %u CPU, displacement removed (control)", mode);
            memset(cpu, 0xcc, sizeof cpu);
            CHECK(nv2a_texture_copy_triangle_depth(&flat, dudv, sizeof dudv, cpu, sizeof cpu, NULL, 0, v[0], v[1], v[2]));
            if (!compare(what, cpu, want, 0)) failed = 1;
            snprintf(what, sizeof what, "mode %u Metal, displacement removed (control)", mode);
            nv2a_metal_invalidate(gpu); memset(gpu, 0xcc, sizeof gpu);
            CHECK(nv2a_metal_draw(&flat, dudv, sizeof dudv, gpu, sizeof gpu, NULL, 0, v, 3, 5) == 1);
            CHECK(nv2a_metal_sync());
            if (!compare(what, gpu, want, 0)) failed = 1;
        }
    }
    if (failed) return 1;
    puts("BUMPENVMAP and BUMPENVMAP_LUMINANCE: CPU and Metal both match the hand-computed displacement; the flat control does not");
    return 0;
}
