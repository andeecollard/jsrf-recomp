/* G42: D3D's fixed-function vertex-state emitters, transcribed.
 *
 * src/nv2a/d3d8_ff_vertex_state.c transcribes the XDK 4134 D3D8 code that
 * writes texgen (0x18F060), texture matrices (0x1957F0), lighting and
 * material (0x195F80), fog (0x195610) and fog colour (0x18EB80). Every
 * expected word below was worked out BY HAND from the disassembly, not by
 * running the transcription (the arithmetic is written out beside each).
 *
 * Positive controls are built in: each group has a vector whose input differs
 * from a neighbour's in one bit and whose expected words differ with it. */
#include "d3d8_ff_vertex_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fail, checks;
#define EQ(got, want, what) do {                                              \
        checks++;                                                             \
        unsigned g_ = (unsigned)(got), w_ = (unsigned)(want);                 \
        if (g_ != w_) { fail = 1;                                             \
            fprintf(stderr, "FAIL %s:%d: %s = 0x%08X, want 0x%08X\n",         \
                    __FILE__, __LINE__, what, g_, w_); }                      \
    } while (0)

static uint32_t W(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

/* The value D3D8FFLights says was written to `method`, the last one if twice;
 * 0xDEADBEEF if not written at all. */
static uint32_t reg(const D3D8FFLights *o, uint32_t method)
{
    uint32_t v = 0xDEADBEEFu;
    for (unsigned i = 0; i < o->n; ++i) if (o->reg[i].method == method) v = o->reg[i].value;
    return v;
}
static unsigned count(const D3D8FFLights *o, uint32_t method)
{
    unsigned n = 0;
    for (unsigned i = 0; i < o->n; ++i) n += o->reg[i].method == method;
    return n;
}

static void test_texgen(void)
{
    uint32_t eye = 7;
    EQ(d3d8_ff_texgen(0, &eye), 0, "PASSTHRU");              EQ(eye, 0, "PASSTHRU eye");
    EQ(d3d8_ff_texgen(0x10000, &eye), 0x8511, "CAMERASPACENORMAL -> NORMAL_MAP");  EQ(eye, 1, "normal eye");
    EQ(d3d8_ff_texgen(0x20001, &eye), 0x2400, "CAMERASPACEPOSITION -> EYE_LINEAR"); EQ(eye, 0, "position eye");
    EQ(d3d8_ff_texgen(0x30000, &eye), 0x8512, "REFLECTIONVECTOR -> REFLECTION_MAP"); EQ(eye, 1, "reflection eye");
    EQ(d3d8_ff_texgen(0x40000, &eye), 0x2401, "OBJECT -> OBJECT_LINEAR");            EQ(eye, 0, "object eye");
    EQ(d3d8_ff_texgen(0x50000, &eye), 0x2402, "SPHERE -> SPHERE_MAP");               EQ(eye, 1, "sphere eye");
    /* Quirks: anything above 0x30000 but 0x40000 is SPHERE_MAP; anything
     * below 0x30000 but 0x10000 is EYE_LINEAR. */
    EQ(d3d8_ff_texgen(0x70000, &eye), 0x2402, "0x70000 -> SPHERE_MAP");
    EQ(d3d8_ff_texgen(0x10001, &eye), 0x8511, "low word ignored");
}

static void test_tex_transforms(void)
{
    D3D8FFTexXformIn in;
    D3D8FFTexXform o;
    memset(&in, 0, sizeof in);
    for (unsigned s = 0; s < 4; ++s)
        for (unsigned i = 0; i < 16; ++i) in.matrix[s][i] = 0x41000000u + 0x100u * s + i;   /* m[i] tagged */
#define M(s, i) (0x41000000u + 0x100u * (s) + (i))
    /* Stage 0: JSRF's cel-shading setup -- COUNT2 with CAMERASPACENORMAL:
     * in = 3, key 0x320, case E: rows (m0 m4 m8 m12) (m1 m5 m9 m13) 0 (0 0 0 1). */
    in.ttf[0] = 2; in.tci[0] = 0x10000;
    /* Stage 1: COUNT2, texcoord set 0 with no size byte: in = 2, key 0x220,
     * case A: rows (m0 m4 0 m8) (m1 m5 0 m9) 0 (0 0 0 1). */
    in.ttf[1] = 2; in.tci[1] = 0;
    /* Stage 2: COUNT3|PROJECTED, set 5 -> shift (5 << 3) & 31 = 8 -> the
     * second size byte (3): in = 3, key 0x331, case G. */
    in.ttf[2] = 0x103; in.tci[2] = 5; in.vs_tex_sizes = 0x0300;
    /* Stage 3: disabled. */
    in.ttf[3] = 0; in.tci[3] = 0x10000;
    EQ(d3d8_ff_tex_transforms(&in, &o), 0, "tx ok");
    EQ(o.emitted, 1, "tx emitted");
    EQ(o.enable[0], 1, "tx en0"); EQ(o.enable[1], 1, "tx en1"); EQ(o.enable[2], 1, "tx en2"); EQ(o.enable[3], 0, "tx en3");
    EQ(o.matrix_written, 7, "tx written");
    EQ(o.key[0], 0x320, "key0"); EQ(o.key[1], 0x220, "key1"); EQ(o.key[2], 0x331, "key2");
    {   const uint32_t e0[16] = { M(0,0), M(0,4), M(0,8), M(0,12),  M(0,1), M(0,5), M(0,9), M(0,13),
                                  0, 0, 0, 0,  0, 0, 0, 0x3F800000 };
        const uint32_t e1[16] = { M(1,0), M(1,4), 0, M(1,8),  M(1,1), M(1,5), 0, M(1,9),
                                  0, 0, 0, 0,  0, 0, 0, 0x3F800000 };
        const uint32_t e2[16] = { M(2,0), M(2,4), M(2,8), M(2,12),  M(2,1), M(2,5), M(2,9), M(2,13),
                                  0, 0, 0, 0,  M(2,2), M(2,6), M(2,10), M(2,14) };
        for (unsigned i = 0; i < 16; ++i) {
            char b[32];
            snprintf(b, sizeof b, "E o[%u]", i); EQ(o.matrix[0][i], e0[i], b);
            snprintf(b, sizeof b, "A o[%u]", i); EQ(o.matrix[1][i], e1[i], b);
            snprintf(b, sizeof b, "G o[%u]", i); EQ(o.matrix[2][i], e2[i], b);
        } }
    /* The other cases, one stage each. */
    in.ttf[0] = 3; in.tci[0] = 0; in.vs_tex_sizes = 0;            /* in 2, 0x230: B */
    in.ttf[1] = 0x103; in.tci[1] = 0;                             /* in 2, 0x231: C */
    in.ttf[2] = 0x104; in.tci[2] = 0;                             /* in 2, 0x241: D */
    in.ttf[3] = 3; in.tci[3] = 0x10000;                           /* in 3, 0x330: F */
    EQ(d3d8_ff_tex_transforms(&in, &o), 0, "tx ok 2");
    {   const uint32_t eB[16] = { M(0,0), M(0,4), 0, M(0,8),  M(0,1), M(0,5), 0, M(0,9),
                                  M(0,2), M(0,6), 0, M(0,10),  0, 0, 0, 0x3F800000 };
        const uint32_t eC[16] = { M(1,0), M(1,4), 0, M(1,8),  M(1,1), M(1,5), 0, M(1,9),
                                  0, 0, 0, 0,  M(1,2), M(1,6), 0, M(1,10) };
        const uint32_t eD[16] = { M(2,0), M(2,4), 0, M(2,8),  M(2,1), M(2,5), 0, M(2,9),
                                  M(2,2), M(2,6), 0, M(2,10),  M(2,3), M(2,7), 0, M(2,11) };
        const uint32_t eF[16] = { M(3,0), M(3,4), M(3,8), M(3,12),  M(3,1), M(3,5), M(3,9), M(3,13),
                                  M(3,2), M(3,6), M(3,10), M(3,14),  0, 0, 0, 0x3F800000 };
        for (unsigned i = 0; i < 16; ++i) {
            char b[32];
            snprintf(b, sizeof b, "B o[%u]", i); EQ(o.matrix[0][i], eB[i], b);
            snprintf(b, sizeof b, "C o[%u]", i); EQ(o.matrix[1][i], eC[i], b);
            snprintf(b, sizeof b, "D o[%u]", i); EQ(o.matrix[2][i], eD[i], b);
            snprintf(b, sizeof b, "F o[%u]", i); EQ(o.matrix[3][i], eF[i], b);
        } }
    /* H: COUNT4 with texgen, 0x340 -> the transpose. */
    in.ttf[0] = 4; in.tci[0] = 0x20000;
    /* Unresolved: COUNT4 with 2 inputs (0x240 -> byte 4 -> address 0) and
     * COUNT1 (0x210, below the table). */
    in.ttf[1] = 4; in.tci[1] = 0;
    in.ttf[2] = 1; in.tci[2] = 0;
    in.ttf[3] = 0;
    EQ(d3d8_ff_tex_transforms(&in, &o), -1, "tx unresolved");
    EQ(o.unresolved, 6, "tx unresolved stages");
    EQ(o.matrix_written, 1, "tx only H written");
    EQ(o.enable[1], 1, "unresolved stage still enabled");
    for (unsigned i = 0; i < 16; ++i) { char b[32]; snprintf(b, sizeof b, "H o[%u]", i);
                                        EQ(o.matrix[0][i], M(0, (i % 4) * 4 + i / 4), b); }
    /* A programmable or pass-through shader: nothing (0x195800). */
    in.vs_flags = 0x10;
    EQ(d3d8_ff_tex_transforms(&in, &o), 0, "tx vs");
    EQ(o.emitted, 0, "tx vs emitted");
    in.vs_flags = 0x2;
    d3d8_ff_tex_transforms(&in, &o);
    EQ(o.emitted, 0, "tx passthrough emitted");
#undef M
}

static void test_fog(void)
{
    D3D8FFFogIn in;
    D3D8FFFog o;
    memset(&in, 0, sizeof in);
    in.equal_scale = 0x46000000;                  /* 8192.0 */
    d3d8_ff_fog(&in, &o);
    EQ(o.enable, 0, "fog off"); EQ(o.params_written, 0, "fog off params");
    /* FOGTABLEMODE NONE: vertex fog from specular alpha, LINEAR (1, 1, 0). */
    in.enable = 1;
    d3d8_ff_fog(&in, &o);
    EQ(o.enable, 1, "fog on"); EQ(o.gen_mode, 0, "NONE gen"); EQ(o.mode, 0x2601, "NONE mode");
    EQ(o.params[0], 0x3F800000, "NONE p0"); EQ(o.params[1], 0x3F800000, "NONE p1"); EQ(o.params[2], 0, "NONE p2");
    /* LINEAR 0..100, range fog off: scale 1/100; p0 = 100 * (1/100) + 1 = 2.0
     * (the double product rounds to 1.0); p1 = -(1/100) -> float 0xBC23D70A;
     * gen PLANAR (2). */
    in.table_mode = 3; in.start = W(0.0f); in.end = W(100.0f);
    d3d8_ff_fog(&in, &o);
    EQ(o.gen_mode, 2, "LINEAR gen"); EQ(o.mode, 0x2601, "LINEAR mode");
    EQ(o.params[0], 0x40000000, "LINEAR p0"); EQ(o.params[1], 0xBC23D70A, "LINEAR p1");
    /* start == end == 5: scale 8192; p0 = 40961 = 0x47200100, p1 = -8192. Range fog: RADIAL (1). */
    in.start = in.end = W(5.0f); in.range_enable = 1;
    d3d8_ff_fog(&in, &o);
    EQ(o.gen_mode, 1, "LINEAR range gen");
    EQ(o.params[0], 0x47200100, "LINEAR eq p0"); EQ(o.params[1], 0xC6000000, "LINEAR eq p1");
    /* EXP, density 0.5: p0 1.5; p1 = 0.5 * K (0xBDB8AA0A) = exponent - 1 = 0xBD38AA0A. */
    in.table_mode = 1; in.density = W(0.5f); in.range_enable = 0;
    d3d8_ff_fog(&in, &o);
    EQ(o.mode, 0x800, "EXP mode"); EQ(o.params[0], 0x3FC00000, "EXP p0"); EQ(o.params[1], 0xBD38AA0A, "EXP p1");
    /* EXP2, density 2: p1 = 2 * 0xBE596D0B = 0xBED96D0B. Table mode 7 takes the same path. */
    in.table_mode = 2; in.density = W(2.0f);
    d3d8_ff_fog(&in, &o);
    EQ(o.mode, 0x801, "EXP2 mode"); EQ(o.params[1], 0xBED96D0B, "EXP2 p1");
    in.table_mode = 7;
    d3d8_ff_fog(&in, &o);
    EQ(o.mode, 0x801, "mode 7 -> EXP2");
    EQ(d3d8_ff_fog_color(0x11223344u), 0x11443322u, "fog colour swaps red and blue");
    EQ(d3d8_ff_fog_color(0x00FFFFFFu), 0x00FFFFFFu, "white");
}

static void test_helpers(void)
{
    float v[3];
    /* rsqrt(1): guess (0xBE800000 - 0x3F800000) >> 1 = 0x3F800000; first step
     * 1.47 - 0.47 = 1.00000003 -> float 1.0; second 0.5 * 1 * (3 - 1) = 1. */
    EQ(W((float)d3d8_ff_rsqrt(1.0f)), 0x3F800000, "rsqrt 1");
    /* rsqrt(4): guess 0x3F000000 (0.5), both steps land on 0.5. */
    EQ(W((float)d3d8_ff_rsqrt(4.0f)), 0x3F000000, "rsqrt 4");
    checks++; if (fabs(d3d8_ff_rsqrt(2.0f) - 0.70710678) > 1e-3) { fail = 1; fprintf(stderr, "FAIL rsqrt 2\n"); }
    v[0] = 0; v[1] = 3; v[2] = 4;
    d3d8_ff_normalize(v);
    checks++; if (fabsf(v[1] - 0.6f) > 1e-3f || fabsf(v[2] - 0.8f) > 1e-3f) { fail = 1; fprintf(stderr, "FAIL normalize\n"); }
}

static void material(uint32_t *m)
{
    /* diffuse (1, .5, .25, .75), ambient (.5 .5 .5 1), specular 0, emissive (.25 0 0 0), power 0 */
    const float f[17] = { 1, .5f, .25f, .75f,  .5f, .5f, .5f, 1,  0, 0, 0, 0,  .25f, 0, 0, 0,  0 };
    for (unsigned i = 0; i < 17; ++i) m[i] = W(f[i]);
}

static void test_lights(void)
{
    D3D8FFLightIn in;
    D3D8FFLights o;
    memset(&in, 0, sizeof in);
    material(in.material);
    for (unsigned i = 0; i < 16; ++i) in.view[i] = W(i % 5 == 0 ? 1.0f : 0.0f);   /* identity */
    in.eye[2] = W(-1.0f);
    in.lighting = 1; in.ambient = 0x00FF8000u; in.list_head = 1; in.nlights = 1;
    /* Light 0: directional, diffuse 1, specular 0, ambient (.5 .25 1), -dir (0 0 -1). */
    in.light[0][0] = 3;
    in.light[0][1] = in.light[0][2] = in.light[0][3] = W(1.0f);
    in.light[0][9] = W(.5f); in.light[0][10] = W(.25f); in.light[0][11] = W(1.0f);
    in.light[0][29] = W(-1.0f);
    EQ(d3d8_ff_lights(&in, &o), 0, "lit ok");
    EQ(o.lit, 1, "lit");
    EQ(reg(&o, 0x294), 1, "LIGHT_CONTROL"); EQ(reg(&o, 0x314), 1, "LIGHTING_ENABLE");
    EQ(reg(&o, 0x17C4), 0, "TWO_SIDE"); EQ(reg(&o, 0x3B8), 1, "SPECULAR_ENABLE (lit path)");
    EQ(reg(&o, 0x298), 0, "COLOR_MATERIAL (COLORVERTEX off)");
    /* Scene ambient, both from the material: r = 255 * (1/255 float) =
     * 1 + 127/2^31; * .5 + .25 = .75000002957 -> float .75 (under half an
     * ulp); g = 128/255 as float = 8421505/2^24 exactly, * .5 = 0x3E808081;
     * b = 0. Emission 0, alpha = diffuse alpha .75. */
    EQ(reg(&o, 0xA10), 0x3F400000, "SCENE_AMBIENT r"); EQ(reg(&o, 0xA14), 0x3E808081, "SCENE_AMBIENT g");
    EQ(reg(&o, 0xA18), 0, "SCENE_AMBIENT b");
    EQ(reg(&o, 0x3A8), 0, "EMISSION r"); EQ(reg(&o, 0x3B0), 0, "EMISSION b");
    EQ(reg(&o, 0x3B4), 0x3F400000, "MATERIAL_ALPHA");
    /* Light 0 colours: ambient = mat.amb * L.amb = (.25 .125 .5); diffuse =
     * L.dif * mat.dif = (1 .5 .25); specular 0. */
    EQ(reg(&o, 0x1000), 0x3E800000, "L0 amb r"); EQ(reg(&o, 0x1004), 0x3E000000, "L0 amb g"); EQ(reg(&o, 0x1008), 0x3F000000, "L0 amb b");
    EQ(reg(&o, 0x100C), 0x3F800000, "L0 dif r"); EQ(reg(&o, 0x1010), 0x3F000000, "L0 dif g"); EQ(reg(&o, 0x1014), 0x3E800000, "L0 dif b");
    EQ(reg(&o, 0x1018), 0, "L0 spec r");
    /* Directional: range 1e30; D = view * (0 0 -1) = (0 0 -1), normalised by
     * rsqrt(1) = 1; H = normalise(D + eye) = normalise(0 0 -2) = rsqrt(4) = .5
     * -> (0 0 -1). */
    EQ(reg(&o, 0x1024), 0x7149F2CA, "L0 range");
    EQ(reg(&o, 0x1028), 0, "L0 H x"); EQ(reg(&o, 0x1030), 0xBF800000, "L0 H z");
    EQ(reg(&o, 0x1034), 0, "L0 D x"); EQ(reg(&o, 0x103C), 0xBF800000, "L0 D z");
    EQ(reg(&o, 0x3BC), 1, "LIGHT_ENABLE_MASK one infinite light");
    EQ(reg(&o, 0x105C), 0xDEADBEEF, "no local position for a directional light");

    /* Add a point and a spot light, and move the view by (10 20 30). */
    in.view[12] = W(10); in.view[13] = W(20); in.view[14] = W(30);
    in.nlights = 3;
    in.light[1][0] = 1; in.light[1][13] = W(1); in.light[1][14] = W(2); in.light[1][15] = W(3);
    in.light[1][19] = W(50); in.light[1][21] = W(1); in.light[1][22] = W(.5f); in.light[1][23] = W(.25f);
    in.light[2][0] = 2; in.light[2][29] = W(-1.0f); in.light[2][33] = W(2.0f); in.light[2][34] = W(.5f);
    in.light[2][30] = W(1); in.light[2][31] = W(2); in.light[2][32] = W(3); in.light[2][19] = W(7);
    d3d8_ff_lights(&in, &o);
    /* Point: P = view * (1 2 3 1) = (11 22 33); range 50; attenuation copied. */
    EQ(reg(&o, 0x10A4), W(50), "L1 range");
    EQ(reg(&o, 0x10DC), W(11), "L1 P x"); EQ(reg(&o, 0x10E0), W(22), "L1 P y"); EQ(reg(&o, 0x10E4), W(33), "L1 P z");
    EQ(reg(&o, 0x10E8), W(1), "L1 att0"); EQ(reg(&o, 0x10F0), W(.25f), "L1 att2");
    /* Spot: position (0 0 0) -> (10 20 30); direction w = 0 so the view's
     * translation drops: (0 0 -1) * scale 2 = (0 0 -2); falloff and w copied. */
    EQ(reg(&o, 0x115C), W(10), "L2 P x"); EQ(reg(&o, 0x1164), W(30), "L2 P z");
    EQ(reg(&o, 0x1140), W(1), "L2 falloff0"); EQ(reg(&o, 0x1148), W(3), "L2 falloff2");
    EQ(reg(&o, 0x114C), 0, "L2 S x"); EQ(reg(&o, 0x1154), W(-2), "L2 S z"); EQ(reg(&o, 0x1158), W(.5f), "L2 S w");
    /* The directional light's direction is not moved by translation either. */
    EQ(reg(&o, 0x103C), 0xBF800000, "L0 D z after translation");
    /* mask: infinite 1 at bits 0-1, local 2 at 2-3, spot 3 at 4-5 = 0x39. */
    EQ(reg(&o, 0x3BC), 0x39, "LIGHT_ENABLE_MASK dir+point+spot");
    EQ(o.types, 0x010101, "types");

    /* Colour material: DIFFUSEMATERIALSOURCE (RS 101, bits 4-5) = COLOR1 with
     * a diffuse vertex colour -> 0x10, and the light's diffuse goes out
     * unmultiplied. Without the vertex colour (flag 0x400) the source is cleared. */
    in.nlights = 1; in.color_vertex = 1; in.mat_source[5] = 1; in.vs_flags = 0x400;
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0x298), 0x10, "COLOR_MATERIAL diffuse=COLOR1");
    EQ(reg(&o, 0x1010), 0x3F800000, "L0 dif g from the vertex (L.dif as is)");
    in.vs_flags = 0;
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0x298), 0, "COLOR1 cleared without a diffuse vertex colour");
    EQ(reg(&o, 0x1010), 0x3F000000, "L0 dif g back to mat * L");
    /* Ambient from the vertex (AMBIENTMATERIALSOURCE, bits 2-3, = COLOR2 with
     * a specular colour, flag 0x800): scene ambient <- material emissive
     * (.25 0 0), emission <- the ambient colour (1.0000000591 -> float 1.0,
     * 8421505/2^24, 0). */
    in.mat_source[5] = 0; in.mat_source[6] = 2; in.vs_flags = 0x800;
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0x298), 0x08, "COLOR_MATERIAL ambient=COLOR2");
    EQ(reg(&o, 0xA10), 0x3E800000, "SCENE_AMBIENT <- emissive r"); EQ(reg(&o, 0xA14), 0, "SCENE_AMBIENT <- emissive g");
    EQ(reg(&o, 0x3A8), 0x3F800000, "EMISSION <- ambient r"); EQ(reg(&o, 0x3AC), 0x3F008081, "EMISSION <- ambient g");
    EQ(reg(&o, 0x1000), 0x3F000000, "L0 amb r from the vertex (L.amb as is)");
    /* Emissive from the vertex (EMISSIVEMATERIALSOURCE, bits 0-1): scene
     * ambient = ambient * mat.amb only, emission = 1. */
    in.mat_source[6] = 0; in.mat_source[7] = 1; in.vs_flags = 0x400;
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0xA10), 0x3F000000, "SCENE_AMBIENT r = 1.00000006 * .5 -> .5"); EQ(reg(&o, 0x3A8), 0x3F800000, "EMISSION 1");
    in.color_vertex = 0; in.mat_source[7] = 0; in.vs_flags = 0;

    /* Two-sided: back ambient/emission/alpha at 0x17A0/0x17B0/0x17AC and back
     * light colours at 0x0C00; each written once. */
    in.two_sided = 1; in.back_ambient = 0x00000000u; material(in.back_material);
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0x17C4), 1, "TWO_SIDE");
    EQ(reg(&o, 0x17A0), 0x3E800000, "BACK_SCENE_AMBIENT r = 0 + emissive .25");
    EQ(reg(&o, 0x17AC), 0x3F400000, "BACK_MATERIAL_ALPHA");
    EQ(reg(&o, 0x0C00), 0x3E800000, "BACK L0 amb r"); EQ(count(&o, 0x0C00), 1, "back written once");
    in.two_sided = 0;

    /* Specular with lighting: 0x195E40 runs (not transcribed) -> unresolved;
     * LIGHT_CONTROL gets LOCALEYE with LOCALVIEWER and a light list. */
    in.specular_enable = 1; in.local_viewer = 1;
    EQ(d3d8_ff_lights(&in, &o), -1, "specular unresolved");
    EQ(o.unresolved, D3D8FF_LUNRES_SPECULAR, "specular bit");
    EQ(reg(&o, 0x294), 0x10001, "LIGHT_CONTROL local eye");
    in.local_viewer = 0; in.specular_enable = 0;

    /* Lighting off: four registers, SPECULAR_ENABLE from device+8 bit 0x40. */
    in.lighting = 0; in.device_flags = 0x40; in.two_sided = 1;
    EQ(d3d8_ff_lights(&in, &o), 0, "unlit ok");
    EQ(o.lit, 0, "unlit"); EQ(o.n, 4, "unlit writes four");
    EQ(reg(&o, 0x314), 0, "unlit LIGHTING"); EQ(reg(&o, 0x3B8), 1, "unlit SPECULAR from device flags");
    EQ(reg(&o, 0x294), 0x20001, "unlit LIGHT_CONTROL"); EQ(reg(&o, 0x17C4), 1, "unlit TWO_SIDE");
    in.device_flags = 0;
    d3d8_ff_lights(&in, &o);
    EQ(reg(&o, 0x3B8), 0, "unlit SPECULAR off");
    /* Lighting on but a programmable shader: the unlit path. */
    in.lighting = 1; in.vs_flags = 0x10;
    d3d8_ff_lights(&in, &o);
    EQ(o.lit, 0, "vs -> unlit path");
}

/* ---- the inverse model-view: 0x190750, 0x190A30, 0x1962B0 --------------- */
static void mat(uint32_t m[16], const float f[16]) { for (unsigned i = 0; i < 16; ++i) m[i] = W(f[i]); }
static void eq12(const uint32_t *got, const uint32_t want[12], const char *what)
{
    char b[96];
    for (unsigned k = 0; k < 12; ++k) { snprintf(b, sizeof b, "%s [%u]", what, k); EQ(got[k], want[k], b); }
}
static void test_inverse(void)
{
    static const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    uint32_t m[16], o[16];
    /* 1. Identity, scaled (NORMALIZENORMALS off). Every cofactor slot is a
     * difference of products of 0s and 1s, none negative, so the zeros are
     * +0; det = 1, rsqrt(1) = 1 (test_helpers), out = the first 3 rows of I. */
    { static const uint32_t want[12] = { 0x3F800000,0,0,0, 0,0x3F800000,0,0, 0,0,0x3F800000,0 };
      mat(m, I);
      EQ(d3d8_ff_inverse(o, m, 1), 0, "inverse I ok"); eq12(o, want, "inverse I"); }
    /* 2. Scale diag(2, 4, .5) with a translation (3, 2, 8) in row 3 (row
     * vectors): det = 2*4*.5 = 4 exactly; det^2 = 16, rsqrt: guess
     * (0xBE800000 - 0x41800000) >> 1 = 0x3E800000 = .25, step 1
     * .25 * (1.47f - .47f*16/16) = .25000000745 -> float .25, step 2
     * .5*.25*(3 - 1) = .25. Cofactors C00 = 4*.5*1 = 2, C11 = 2*.5 = 1,
     * C22 = 2*4 = 8, times .25: the inverse's rows .5, .25, 2. No entry is
     * negative, so every zero is +0. The translation only reaches row 3,
     * which the guest does not send. */
    { static const float M[16] = { 2,0,0,0, 0,4,0,0, 0,0,.5f,0, 3,2,8,1 };
      static const uint32_t want[12] = { 0x3F000000,0,0,0, 0,0x3E800000,0,0, 0,0,0x40000000,0 };
      static const uint32_t adj[12]  = { 0x40000000,0,0,0, 0,0x3F800000,0,0, 0,0,0x41000000,0 };
      mat(m, M);
      EQ(d3d8_ff_inverse(o, m, 1), 0, "inverse S ok"); eq12(o, want, "inverse S");
      /* NORMALIZENORMALS on: no scale, the cofactors with det's sign (+). */
      EQ(d3d8_ff_inverse(o, m, 0), 0, "adjugate S ok"); eq12(o, adj, "adjugate S"); }
    /* 3. diag(-2, 4, .5, 1): det = -4. Traced slot by slot (the names in
     * d3d8_ff_inverse): t28 = 0*0 - .5*0 = +0, t2c = +0, s18 = .5, e1 = e2 =
     * e3 = +0; s1c = .5*4 = 2; s04 = .5*-2 = -1; s44 = (+0 - (-0*0)) + -8*1 = -8;
     * s00 = (0*-2 = -0) - 0 = -0, + 0 = +0; s08 = +0 - (0*-2 = -0) = +0;
     * s3c = (-0*0) - (-0*1) = -0 - -0 = +0; s4c = (-0*0) - (-8*0) = +0; the
     * rest +0. det = ((0 + 0) + 0) + 2*-2 = -4.
     * Unscaled, every slot gets det's sign bit XORed in: +0 -> -0. Scaled,
     * s = -(rsqrt(16) = .25): 2 -> -.5, -1 -> .25, -8 -> 2, +0 -> -0. */
    { static const float M[16] = { -2,0,0,0, 0,4,0,0, 0,0,.5f,0, 0,0,0,1 };
      static const uint32_t adj[12] = { 0xC0000000,0x80000000,0x80000000,0x80000000,
                                        0x80000000,0x3F800000,0x80000000,0x80000000,
                                        0x80000000,0x80000000,0x41000000,0x80000000 };
      static const uint32_t inv[12] = { 0xBF000000,0x80000000,0x80000000,0x80000000,
                                        0x80000000,0x3E800000,0x80000000,0x80000000,
                                        0x80000000,0x80000000,0x40000000,0x80000000 };
      mat(m, M);
      EQ(d3d8_ff_inverse(o, m, 0), 0, "adjugate N ok"); eq12(o, adj, "adjugate N");
      EQ(d3d8_ff_inverse(o, m, 1), 0, "inverse N ok"); eq12(o, inv, "inverse N"); }
    /* 4. D3D's rsqrt is approximate, and the inverse inherits it: diag(3,3,3,1)
     * has det 27, det^2 = 729 (0x44364000). Guess (0xBE800000 - 0x44364000) >> 1
     * = 0x3D24E000 = .0402527; step 1 in double: .47f*729 = 342.6299991,
     * times y0^2, 1.47f minus that, times y0 = .0368249 -> float 0x3D16D5BC;
     * step 2: .5 * y1 * (3 - 729 y1^2) = .0370352184 -> float 0x3D17B23E.
     * Cofactors 9, so the diagonal is float(9 * 0x3D17B23E) = 0x3EAAA886 =
     * .33331698 -- not 1/3 (0x3EAAAAAB): 5e-5 relative, beyond the check's
     * 1e-5 tolerance, which is why only an exact transcription will do. */
    { static const float M[16] = { 3,0,0,0, 0,3,0,0, 0,0,3,0, 0,0,0,1 };
      static const uint32_t want[12] = { 0x3EAAA886,0,0,0, 0,0x3EAAA886,0,0, 0,0,0x3EAAA886,0 };
      mat(m, M);
      EQ(d3d8_ff_inverse(o, m, 1), 0, "inverse 3 ok"); eq12(o, want, "inverse 3"); }
    /* 5. Singular: rows 0 and 1 equal, integers throughout, so det is exactly
     * 0 and fcomp finds it equal to [0x1C43D0] = +0: -1, output untouched. */
    { static const float M[16] = { 1,2,3,0, 1,2,3,0, 0,0,1,0, 0,0,0,1 };
      mat(m, M);
      for (unsigned k = 0; k < 16; ++k) o[k] = 0xA5A5A5A5u;
      EQ((uint32_t)d3d8_ff_inverse(o, m, 1), 0xFFFFFFFFu, "singular -> -1");
      EQ(o[0], 0xA5A5A5A5u, "singular leaves out"); EQ(o[11], 0xA5A5A5A5u, "singular leaves out 11");
      EQ((uint32_t)d3d8_ff_inverse(o, m, 0), 0xFFFFFFFFu, "singular unscaled -> -1"); }
    /* 6. 0x190750: rows of a times b, in float. (1 2 3 4) against rows
     * (1 0 0 0) (0 1 0 0) (0 0 1 0) (10 20 30 1) = (1 + 40, 2 + 80, 3 + 120, 4). */
    { static const float A[16] = { 1,2,3,4, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      static const float B[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 };
      uint32_t a[16], b[16];
      mat(a, A); mat(b, B);
      d3d8_ff_matmul(o, a, b);
      EQ(o[0], W(41.0f), "matmul r0c0"); EQ(o[1], W(82.0f), "matmul r0c1");
      EQ(o[2], W(123.0f), "matmul r0c2"); EQ(o[3], W(4.0f), "matmul r0c3"); }
    /* ... and the order: row (1e8, 1, -1e8, 0) against b = rows (1 0 0 0) x3
     * sums ((1e8 + 1) + -1e8) in float: 1e8 + 1 rounds back to 1e8 (ulp 8),
     * so column 0 is 0, not 1. Summed (1e8 + -1e8) + 1 it would be 1. */
    { static const float A[16] = { 1e8f,1,-1e8f,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      static const float B[16] = { 1,0,0,0, 1,0,0,0, 1,0,0,0, 0,0,0,1 };
      uint32_t a[16], b[16];
      mat(a, A); mat(b, B);
      d3d8_ff_matmul(o, a, b);
      EQ(o[0], 0, "matmul absorbs: ((1e8 + 1) - 1e8) = 0"); }
    /* 7. The updater 0x1962B0: WORLD = 2I with translation (1, 2, 3), VIEW =
     * I with translation (0, 0, 5): WORLD*VIEW rows (2 0 0 0) (0 2 0 0)
     * (0 0 2 0) (1 2 8 1), all exact. det = 8, det^2 = 64, rsqrt(64) = .125
     * exactly (as for 16: the guess 0x3E000000 is right and step 1 rounds
     * back to it); cofactors 4, so rows .5 -- or 4 with NORMALIZENORMALS. */
    {   static const float Wd[16] = { 2,0,0,0, 0,2,0,0, 0,0,2,0, 1,2,3,1 };
        static const float Vw[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,5,1 };
        static const uint32_t want[12] = { 0x3F000000,0,0,0, 0,0x3F000000,0,0, 0,0,0x3F000000,0 };
        static const uint32_t adj[12]  = { 0x40800000,0,0,0, 0,0x40800000,0,0, 0,0,0x40800000,0 };
        D3D8FFInvMVIn in; D3D8FFInvMV io;
        memset(&in, 0, sizeof in);
        mat(in.world, Wd); mat(in.view, Vw);
        in.lighting = 1; in.dirty = 0x200;
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.emitted, 1, "imv emitted"); EQ(io.inverse_written, 1, "imv written"); EQ(io.singular, 0, "imv regular");
        EQ(io.modelview[12], W(1.0f), "imv WV[3][0]"); EQ(io.modelview[14], W(8.0f), "imv WV[3][2]");
        eq12(io.inverse, want, "imv");
        in.normalize = 1;                                    /* NORMALIZENORMALS on */
        d3d8_ff_inverse_modelview(&in, &io); eq12(io.inverse, adj, "imv normalised");
        in.normalize = 0; in.lighting = 0;                   /* no user: MODELVIEW only */
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.emitted, 1, "unlit emitted"); EQ(io.inverse_written, 0, "unlit no 0x580");
        in.eye_normal_mask = 1;                              /* NORMAL_MAP texgen on stage 0 */
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.inverse_written, 1, "texgen needs 0x580"); eq12(io.inverse, want, "imv texgen");
        in.dirty = 0x80000200u;                              /* bit 31: nothing at all */
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.emitted, 0, "dirty bit 31"); EQ(io.inverse_written, 0, "dirty bit 31 no 0x580");
        in.dirty = 0x200; in.vs_flags = 0x10;                /* programmable: nothing */
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.emitted, 0, "programmable");
        in.vs_flags = 0; in.world[0] = 0;                    /* WORLD row 0 all zero: singular */
        d3d8_ff_inverse_modelview(&in, &io);
        EQ(io.inverse_written, 1, "singular written"); EQ(io.singular, 1, "singular flagged");
    }
}

int main(void)
{
    test_texgen();
    test_tex_transforms();
    test_fog();
    test_helpers();
    test_lights();
    test_inverse();
    printf("d3d8_ff_vertex_state: %d checks, %s\n", checks, fail ? "FAILED" : "all pass");
    return fail;
}
