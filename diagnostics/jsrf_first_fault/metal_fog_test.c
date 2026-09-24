/* G53: FOG AND THE FINAL COMBINER, three references deep.
 *
 * 1. THE MATHS. nv2a_fog_factor on the params D3D itself writes
 *    (d3d8_ff_fog, the transcription of the title's fog updater) must give
 *    D3D's documented curves: LINEAR (end - d)/(end - start), EXP e^-(d*rho),
 *    EXP2 e^-(d*rho)^2 -- the three FOGTABLEMODEs -- and the _ABS modes the
 *    same curve on |d|.
 * 2. THE FINAL COMBINER, on the CPU. nv2a_final_combine against a direct
 *    evaluation of D3D's fog program (d3d8_ff_final_combiner's words:
 *    lerp(fog colour, colour, factor), with and without the specular sum).
 * 3. THE METAL SHADER against the CPU rasteriser, which is (2) and the
 *    factor per pixel from the interpolated coordinate: a quad whose fog
 *    coordinate runs across it, for D3D's fog programs in every fog mode and
 *    for arbitrary final-combiner programs (every register, both
 *    complements, the clamp, EF_PROD, V1R0_SUM, C0/C1). Within one 565 step
 *    over the whole target; with the fog colour perturbed the Metal image must
 *    differ (the positive control: fog is visible in this scene at all).
 *
 * `vsh` as the first argument draws every quad through a guest vertex program
 * that writes oFog from v5 -- the player's programmable path -- instead of the
 * fixed `vs`. Needs a GPU. */
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include "nv2a_vsh.h"
#include "d3d8_ff_combiner.h"
#include "d3d8_ff_vertex_state.h"
#include "vsh_encode.h"
#include "nv2a_ff.h"
#include "texture_copy_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float f_of(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static uint32_t w_of(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

/* ---- 1. the fog factor ---------------------------------------------------- */
static void factor_tests(void)
{
    static const float ds[] = { 0.0f, 1.0f, 12.5f, 40.0f, 99.0f, 150.0f, 400.0f, 1000.0f };
    D3D8FFFogIn in; D3D8FFFog out;
    double worst[3] = { 0, 0, 0 };
    memset(&in, 0, sizeof in);
    in.enable = 1; in.range_enable = 0; in.equal_scale = w_of(8192.0f);
    in.start = w_of(20.0f); in.end = w_of(300.0f); in.density = w_of(0.0125f);
    for (unsigned t = 1; t <= 3; ++t) {
        in.table_mode = t;
        d3d8_ff_fog(&in, &out);
        for (unsigned k = 0; k < sizeof ds / sizeof ds[0]; ++k) {
            double d = ds[k], want, got = nv2a_fog_factor(out.mode, f_of(out.params[0]), f_of(out.params[1]), (float)d);
            if (t == 3) want = (300.0 - d) / (300.0 - 20.0);
            else if (t == 1) want = exp(-d * 0.0125);
            else want = exp(-(d * 0.0125) * (d * 0.0125));
            want = want < 0 ? 0 : want > 1 ? 1 : want;
            if (fabs(got - want) > worst[t - 1]) worst[t - 1] = fabs(got - want);
        }
    }
    printf("  fog factor against D3D's curves (params from d3d8_ff_fog): worst |error| LINEAR %.2e EXP %.2e EXP2 %.2e\n",
           worst[2], worst[0], worst[1]);
    CHECK(worst[2] < 1e-5 && worst[0] < 1e-5 && worst[1] < 1e-5, "fog factor: LINEAR, EXP and EXP2 are D3D's curves");
    in.table_mode = 3; d3d8_ff_fog(&in, &out);
    CHECK(nv2a_fog_factor(0x804, f_of(out.params[0]), f_of(out.params[1]), -40.0f)
          == nv2a_fog_factor(0x2601, f_of(out.params[0]), f_of(out.params[1]), 40.0f),
          "fog factor: LINEAR_ABS on -d is LINEAR on d");
    CHECK(nv2a_fog_factor(0x2601, f_of(out.params[0]), f_of(out.params[1]), INFINITY)
          == nv2a_fog_factor(0x2601, f_of(out.params[0]), f_of(out.params[1]), 0.0f),
          "fog factor: a non-finite coordinate is 0, as xemu has it");
    CHECK(nv2a_fog_factor(0x1234, 5.0f, 5.0f, 5.0f) == 1.0f, "fog factor: an unknown mode is no fog");
    in.table_mode = 0; d3d8_ff_fog(&in, &out);        /* NONE: the coordinate IS the factor */
    CHECK(fabsf(nv2a_fog_factor(out.mode, f_of(out.params[0]), f_of(out.params[1]), 0.375f) - 0.375f) < 1e-6f,
          "fog factor: FOGTABLEMODE NONE passes specular alpha through");
}

/* ---- 2. the final combiner on the CPU ------------------------------------- */
static void final_cpu_tests(void)
{
    uint32_t cw0, cw1;
    NV2ATextureCopy s; float regs[16][4], out[4];
    int ok = 1;
    for (unsigned spec = 0; spec < 2; ++spec) {
        d3d8_ff_final_combiner(1, spec, 0, 0, &cw0, &cw1);
        CHECK(!nv2a_final_combiner_supported(cw0, cw1), "D3D's fog program %08X/%08X is modelled", cw0, cw1);
        memset(&s, 0, sizeof s);
        s.final_general = 1; s.final_cw0 = cw0; s.final_cw1 = cw1; s.fog_color = 0x00204080u;  /* ABGR: r=0x80 g=0x40 b=0x20 */
        for (unsigned t = 0; t < 50; ++t) {
            float f = (float)t / 49.0f, col[3] = { 0.9f, 0.3f * f, 0.55f }, sp[3] = { 0.1f, 0.2f, 0.05f * t };
            memset(regs, 0, sizeof regs);
            for (int k = 0; k < 3; ++k) { regs[12][k] = col[k]; regs[5][k] = sp[k]; }
            regs[12][3] = 0.625f;
            nv2a_final_combine(&s, regs, f, out);
            for (int k = 0; k < 3; ++k) {
                float c = spec ? fminf(1, col[k] + sp[k]) : col[k];
                float fc = (float)((s.fog_color >> (8 * k)) & 255) / 255.0f;
                float want = f * c + (1 - f) * fc;
                if (fabsf(out[k] - want) > 1e-6f) ok = 0;
            }
            if (out[3] != 0.625f) ok = 0;
        }
    }
    CHECK(ok, "D3D's fog programs: lerp(fog colour, colour [+ specular], factor), alpha R0.a");
    /* The two fog-off programs through the general path equal the old tail. */
    memset(&s, 0, sizeof s); s.final_general = 1; s.final_cw0 = 0xE; s.final_cw1 = 0x1C80;
    memset(regs, 0, sizeof regs);
    regs[12][0] = 0.7f; regs[12][1] = 0.2f; regs[12][2] = 0.9f; regs[12][3] = 0.4f;
    regs[5][0] = 0.5f; regs[5][1] = 0.1f; regs[5][2] = 0.3f;
    nv2a_final_combine(&s, regs, 1.0f, out);
    CHECK(out[0] == 1.0f && fabsf(out[1] - 0.3f) < 1e-6f && out[2] == 1.0f && out[3] == 0.4f,
          "the specular-add program through the general path: R0 + V1 clamped, alpha R0.a");
    CHECK(nv2a_final_combiner_supported(0x06000000u, 0x1C80u) != NULL, "a final input naming register 6 is refused");
    CHECK(nv2a_final_combiner_supported(0x4C000000u, 0x1C80u) != NULL, "a signed final-input mapping is refused");
    CHECK(nv2a_final_combiner_supported(0x0Cu, 0x0E0C1C80u) != NULL, "EF_PROD reading V1R0_SUM is refused");
}

/* ---- 3. Metal against the CPU rasteriser ---------------------------------- */
#define W 96u
#define H 64u
static uint8_t tgt_m[W * H * 2], tgt_c[W * H * 2], dep[W * H * 4];
static int use_vsh;
static uint32_t vsh_words[8][4];
static float vsh_consts[192][4];
static void build_vsh(void)
{
    /* MOV oPos,v0; MOV oD0,v3; MOV oD1,v4; MOV oFog,v5 */
    static const unsigned io[4] = { 0, 3, 4, 5 };
    for (int i = 0; i < 4; ++i) {
        VshIns x; memset(&x, 0, sizeof x);
        x.mac = 1; x.input_index = io[i];
        x.a.mux = 2; x.a.swz = SWZ_ID; x.b.mux = 2; x.b.swz = SWZ_ID; x.c.mux = 2; x.c.swz = SWZ_ID;
        x.out_mask = io[i] == 5 ? 0x8 : 0xF; x.out_reg = io[i]; x.final = i == 3;
        vsh_encode(vsh_words[i], &x);
    }
}
static void quad(float v[6][16][4], float fog0, float fog1)
{
    const float P[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    memset(v, 0, sizeof(float) * 6 * 16 * 4);
    for (int j = 0; j < 6; ++j) {
        float u = P[j][0], t = P[j][1];
        v[j][0][0] = (float)W * u; v[j][0][1] = (float)H * t; v[j][0][2] = 0.25f * 16777215.0f; v[j][0][3] = 1.0f;
        v[j][3][0] = 0.2f + 0.7f * u; v[j][3][1] = 0.9f - 0.5f * t; v[j][3][2] = 0.4f * u + 0.3f * t; v[j][3][3] = 0.3f + 0.6f * t;
        v[j][4][0] = 0.3f * t; v[j][4][1] = 0.5f * u; v[j][4][2] = 0.2f; v[j][4][3] = 0.8f * u;
        v[j][5][0] = fog0 + (fog1 - fog0) * u;          /* the fog coordinate runs left to right */
    }
}
static void state(NV2ATextureCopy *s, uint32_t cw0, uint32_t cw1, uint32_t mode, float p0, float p1, uint32_t color)
{
    memset(s, 0, sizeof *s);
    s->clip_w = W; s->clip_h = H; s->target_pitch = W * 2; s->target_bpp = 2; s->depth_pitch = W * 4;
    s->z_clip_min = 0.0f; s->z_clip_max = 16777215.0f;
    s->untextured = 1; s->modulate = 1;
    /* one stage: R0 = V0 (diffuse), colour and alpha */
    s->combiner_count = 1;
    s->color_icw[0] = 0x04200000u; s->alpha_icw[0] = 0x14200000u;   /* A = V0, B = 1 (ZERO inverted) */
    s->color_ocw[0] = 0x000000C0u; s->alpha_ocw[0] = 0x000000C0u;   /* AB -> R0 */
    s->final_general = !((cw0 == 0xC || cw0 == 0xE) && cw1 == 0x1C80);
    s->add_specular = !s->final_general && cw0 == 0xE;
    s->final_cw0 = cw0; s->final_cw1 = cw1;
    s->fog_enable = 1; s->fog_mode = mode; s->fog_p0 = p0; s->fog_p1 = p1; s->fog_color = color;
    s->spec_fog_c0 = 0x80FF8040u; s->spec_fog_c1 = 0x40204060u;
}
static int draw_metal(NV2ATextureCopy *s, const float v[6][16][4])
{
    memset(tgt_m, 0x33, sizeof tgt_m); memset(dep, 0, sizeof dep);
    nv2a_metal_invalidate(NULL);
    if (use_vsh) {
        if (!nv2a_metal_vsh_ready((const uint32_t (*)[4])vsh_words, 4, (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5))) {
            printf("vertex program refused\n"); return 0;
        }
        nv2a_metal_vsh_constants((const float (*)[4])vsh_consts);
    }
    if (nv2a_metal_draw(s, NULL, 0, tgt_m, sizeof tgt_m, dep, sizeof dep, (const float (*)[16][4])v, 6, 5) < 0) {
        printf("draw rejected: %s\n", nv2a_metal_last_reject()); return 0;
    }
    nv2a_metal_sync();
    return 1;
}
static void draw_cpu(NV2ATextureCopy *s, const float v[6][16][4])
{
    memset(tgt_c, 0x33, sizeof tgt_c);
    nv2a_texture_copy_triangle(s, NULL, 0, tgt_c, sizeof tgt_c, v[0], v[1], v[2]);
    nv2a_texture_copy_triangle(s, NULL, 0, tgt_c, sizeof tgt_c, v[3], v[4], v[5]);
}
static unsigned diff565(unsigned *maxstep, unsigned *covered)
{
    unsigned bad = 0; *maxstep = 0; *covered = 0;
    for (unsigned i = 0; i < W * H; ++i) {
        uint16_t a = (uint16_t)(tgt_m[2 * i] | tgt_m[2 * i + 1] << 8), b = (uint16_t)(tgt_c[2 * i] | tgt_c[2 * i + 1] << 8);
        int dr = abs((a >> 11) - (b >> 11)), dg = abs(((a >> 5) & 63) - ((b >> 5) & 63)), db = abs((a & 31) - (b & 31));
        unsigned m = (unsigned)(dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db));
        if (b != 0x3333) ++*covered;
        if (m > *maxstep) *maxstep = m;
        if (m > 1) ++bad;
    }
    return bad;
}
static unsigned rng = 0xF06F06u;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 7; }
static void metal_tests(void)
{
    static const uint32_t modes[] = { 0x2601, 0x800, 0x801, 0x804, 0x802, 0x803 };
    static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 12, 13, 14, 15 };
    float v[6][16][4];
    NV2ATextureCopy s;
    unsigned bad, step, cov, cases = 0, worst = 0;
    D3D8FFFogIn in; D3D8FFFog lin, ex, ex2;
    memset(&in, 0, sizeof in); in.enable = 1; in.equal_scale = w_of(8192.0f);
    in.start = w_of(10.0f); in.end = w_of(200.0f); in.density = w_of(0.01f);
    in.table_mode = 3; d3d8_ff_fog(&in, &lin);
    in.table_mode = 1; d3d8_ff_fog(&in, &ex);
    in.table_mode = 2; d3d8_ff_fog(&in, &ex2);
    for (unsigned spec = 0; spec < 2; ++spec)
        for (unsigned mi = 0; mi < 6; ++mi) {
            const D3D8FFFog *pf = mi % 3 == 0 ? &lin : mi % 3 == 1 ? &ex : &ex2;
            state(&s, spec ? 0x130E0300u : 0x130C0300u, 0x1C80u, modes[mi], f_of(pf->params[0]), f_of(pf->params[1]), 0x00C08040u);
            quad(v, mi >= 3 ? -250.0f : 0.0f, 250.0f);
            if (!draw_metal(&s, v)) { ++fails; continue; }
            draw_cpu(&s, v);
            bad = diff565(&step, &cov); ++cases; if (step > worst) worst = step;
            if (bad || cov < W * H - 2 * W) printf("  D3D fog program spec=%u mode %X: %u px over one step (max %u), covered %u\n",
                                                   spec, modes[mi], bad, step, cov);
            if (bad) ++fails;
        }
    for (unsigned a = 0; a < 24; ++a) {
        uint32_t w0 = 0, w1 = 0;
        for (int k = 0; k < 4; ++k) w0 = (w0 << 8) | (rnd() & 0x30u) | regs[rnd() % 11];
        for (int k = 0; k < 3; ++k) {
            uint32_t r = regs[rnd() % 11];
            if (k < 2) while (r == 14 || r == 15) r = regs[rnd() % 11];
            w1 = (w1 << 8) | (rnd() & 0x30u) | r;
        }
        w1 = (w1 << 8) | (rnd() & 0xE0u);
        if (nv2a_final_combiner_supported(w0, w1)) { printf("test bug: %08X %08X\n", w0, w1); ++fails; continue; }
        state(&s, w0, w1, modes[a % 6], f_of(lin.params[0]), f_of(lin.params[1]), rnd() * 2654435761u);
        if (a % 6 == 1 || a % 6 == 4) { s.fog_p0 = f_of(ex.params[0]); s.fog_p1 = f_of(ex.params[1]); }
        if (a % 6 == 2 || a % 6 == 5) { s.fog_p0 = f_of(ex2.params[0]); s.fog_p1 = f_of(ex2.params[1]); }
        s.fog_enable = a % 5 != 0;
        quad(v, -20.0f, 260.0f);
        if (!draw_metal(&s, v)) { ++fails; continue; }
        draw_cpu(&s, v);
        bad = diff565(&step, &cov); ++cases; if (step > worst) worst = step;
        if (bad) { printf("  arbitrary %08X %08X mode %X fog %u: %u px over one step (max %u)\n", w0, w1, s.fog_mode,
                          s.fog_enable, bad, step); ++fails; }
    }
    printf("  final combiner + fog, Metal (%s vertex path) against the CPU rasteriser: %u cases, worst %u step(s)\n",
           use_vsh ? "guest program" : "fixed", cases, worst);
    CHECK(worst <= 1 && cases == 36, "Metal final combiner and fog: every case within one 565 step of the CPU reference");
    /* THE SPECULAR FOG PROGRAM AT NO FOG IS THE SPECULAR-ADD PROGRAM, BIT FOR
     * BIT (the player's characters, washed pale in the first fog build):
     * CW0 130E0300 CW1 1C80 with a non-zero specular and fog factor 1 --
     * FOG_ENABLE, LINEAR (2, -0.00025), coordinate 0 everywhere -- against
     * the fog-off program 0xE. The Metal images and the CPU images must each
     * be byte-identical, and the Metal one within a step of the CPU one. */
    {   static uint8_t fog_m[W * H * 2], fog_c[W * H * 2];
        NV2ATextureCopy g, e;
        state(&g, 0x130E0300u, 0x1C80u, 0x2601u, 2.0f, -0.00025f, 0x005E77A5u);
        state(&e, 0xEu, 0x1C80u, 0x2601u, 2.0f, -0.00025f, 0x005E77A5u);
        CHECK(g.final_general && !g.add_specular && !e.final_general && e.add_specular,
              "130E0300 takes the general final combiner, 0xE the specular-add tail");
        quad(v, 0.0f, 0.0f);
        if (!draw_metal(&g, v)) ++fails;
        memcpy(fog_m, tgt_m, sizeof tgt_m);
        draw_cpu(&g, v); memcpy(fog_c, tgt_c, sizeof tgt_c);
        if (!draw_metal(&e, v)) ++fails;
        draw_cpu(&e, v);
        {   unsigned spec_px = 0;
            for (unsigned i = 0; i < W * H; ++i) if (tgt_m[2 * i] != 0x33 || tgt_m[2 * i + 1] != 0x33) ++spec_px;
            CHECK(spec_px > W * H / 2, "the quad is drawn (%u px)", spec_px); }
        CHECK(!memcmp(fog_m, tgt_m, sizeof tgt_m), "Metal: CW0 130E0300 CW1 1C80 at fog factor 1 == the 0xE program, bit for bit");
        CHECK(!memcmp(fog_c, tgt_c, sizeof tgt_c), "CPU: CW0 130E0300 CW1 1C80 at fog factor 1 == the 0xE program, bit for bit");
        /* And the specular is really in it: the same draw without specular differs. */
        {   float v2[6][16][4]; memcpy(v2, v, sizeof v2);
            for (int j = 0; j < 6; ++j) v2[j][4][0] = v2[j][4][1] = v2[j][4][2] = 0;
            draw_metal(&g, (const float (*)[16][4])v2);
            CHECK(memcmp(fog_m, tgt_m, sizeof tgt_m) != 0, "CONTROL: with the specular zeroed the image changes, so V1 is in the sum"); }
    }
    /* CONTROL: the fog colour moved, the Metal image must move with it. */
    state(&s, 0x130C0300u, 0x1C80u, 0x2601, f_of(lin.params[0]), f_of(lin.params[1]), 0x00C08040u);
    quad(v, 0.0f, 250.0f);
    draw_cpu(&s, v);
    s.fog_color = 0x0040C080u;
    draw_metal(&s, v);
    bad = diff565(&step, &cov);
    CHECK(bad > (W * H) / 4, "CONTROL: with the fog colour changed the fogged quad differs (%u px)", bad);
}

/* ---- 4. THROUGH THE GATE, with Rokkaku-dai's own numbers ---------------
 * The first three sections build NV2ATextureCopy by hand, which is how a
 * second refusal (FOG_ENABLE, further down nv2a_texture_copy_prepare) hid
 * from them. This goes through prepare with the state the player's
 * FOG-TRACE recorded: CW0 130C0300 / 130E0300, CW1 1C80, FOG_ENABLE 1,
 * LINEAR, GEN 2 (planar), COLOR 005E77A5, PARAMS 2 -0.00025 0, PLANE
 * 0 0 1 0. Then the fog coordinate for eye z = 1000 through the
 * fixed-function unit (model-view 0x480, FOG_PLANE) and its factor: 0.75. */
static void put_f(uint32_t *m, unsigned byte, float f) { memcpy(&m[byte / 4], &f, 4); }
static void gate_tests(void)
{
    static uint32_t m[2048];
    NV2ATextureCopy s; const char *err; float regs[16][4], out[4];
    copy_methods(m, 3, 2, 8, 20, 4);
    modulate_methods(m);
    CHECK(!nv2a_texture_copy_prepare(m, &s) && !s.final_general, "gate: the fog-off baseline is accepted");
    m[0x288 / 4] = 0x130C0300u; m[0x28C / 4] = 0x1C80u;
    m[0x2A4 / 4] = 1; m[0x29C / 4] = 0x2601u; m[0x2A0 / 4] = 2; m[0x2A8 / 4] = 0x005E77A5u;
    put_f(m, 0x9C0, 2.0f); put_f(m, 0x9C4, -0.00025f); put_f(m, 0x9C8, 0.0f);
    put_f(m, 0x9D0, 0.0f); put_f(m, 0x9D4, 0.0f); put_f(m, 0x9D8, 1.0f); put_f(m, 0x9DC, 0.0f);
    err = nv2a_texture_copy_prepare(m, &s);
    CHECK(!err, "gate: Rokkaku's fogged state is ACCEPTED by nv2a_texture_copy_prepare (%s)", err ? err : "accepted");
    CHECK(!err && s.final_general && s.final_cw0 == 0x130C0300u && s.fog_enable && s.fog_mode == 0x2601u
          && s.fog_p0 == 2.0f && s.fog_p1 == -0.00025f && s.fog_color == 0x005E77A5u,
          "gate: the prepared state carries the final combiner, FOG_ENABLE, LINEAR and params 2, -0.00025");
    m[0x288 / 4] = 0x130E0300u;
    err = nv2a_texture_copy_prepare(m, &s);
    CHECK(!err && s.final_general && !s.add_specular, "gate: the specular fog program too (%s)", err ? err : "accepted");
    /* The colour: FOG_COLOR's parameter is ABGR (xemu, SET_FOG_COLOR: red is
     * the low byte), so 0x005E77A5 is (165, 119, 94) -- the orange haze. A
     * fully fogged pixel is exactly that. */
    memset(regs, 0, sizeof regs); regs[12][0] = 1; regs[12][1] = 1; regs[12][2] = 1;
    s.final_cw0 = 0x130C0300u;
    nv2a_final_combine(&s, regs, 0.0f, out);
    CHECK(fabsf(out[0] * 255 - 165) < 0.01f && fabsf(out[1] * 255 - 119) < 0.01f && fabsf(out[2] * 255 - 94) < 0.01f,
          "fog colour 005E77A5 is (165,119,94): %.1f %.1f %.1f", out[0] * 255, out[1] * 255, out[2] * 255);
    /* Eye z = 1000 through the fixed-function unit: identity model-view,
     * PLANE (0,0,1,0), GEN PLANAR. d = 1000, f = 2 + 1000 * -0.00025 - 1. */
    {   float in[16][4], ff_out[16][4], eye[4], d, f;
        static const float I4[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        static const float T[16] = { 1,0,0,5, 0,1,0,-3, 0,0,1,400, 0,0,0,1 };   /* model-view with a z translation */
        for (unsigned k = 0; k < 16; ++k) put_f(m, 0x480 + 4 * k, I4[k]);
        for (unsigned k = 0; k < 16; ++k) put_f(m, 0x680 + 4 * k, I4[k]);
        memset(in, 0, sizeof in); in[0][0] = 3; in[0][1] = -2; in[0][2] = 1000; in[0][3] = 1;
        d = nv2a_ff_fog_coord(m, nv2a_ff_fog_source(m), (const float (*)[4])in, eye);
        f = nv2a_fog_factor(0x2601u, 2.0f, -0.00025f, d);
        CHECK(d == 1000.0f && fabsf(f - 0.75f) < 1e-6f, "eye z 1000, PLANAR, LINEAR (2, -0.00025): d %g, f %g (0.75)", d, f);
        nv2a_ff_vertex(m, (const float (*)[4])in, ff_out);
        CHECK(ff_out[5][0] == 1000.0f, "nv2a_ff_vertex writes that coordinate to output slot 5 (%g)", ff_out[5][0]);
        for (unsigned k = 0; k < 16; ++k) put_f(m, 0x480 + 4 * k, T[k]);
        in[0][2] = 600;
        d = nv2a_ff_fog_coord(m, nv2a_ff_fog_source(m), (const float (*)[4])in, eye);
        CHECK(d == 1000.0f && eye[2] == 1000.0f, "the model-view's z translation reaches the eye z (600 + 400 = %g)", d);
        m[0x2A0 / 4] = 1;                                                       /* RADIAL */
        d = nv2a_ff_fog_coord(m, nv2a_ff_fog_source(m), (const float (*)[4])in, eye);
        CHECK(fabsf(d - sqrtf(8.0f * 8.0f + 5.0f * 5.0f + 1000.0f * 1000.0f)) < 1e-2f, "RADIAL: |eye| (%g)", d);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "vsh")) { use_vsh = 1; build_vsh(); }
    gate_tests();
    factor_tests();
    final_cpu_tests();
    metal_tests();
    nv2a_metal_report();
    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
