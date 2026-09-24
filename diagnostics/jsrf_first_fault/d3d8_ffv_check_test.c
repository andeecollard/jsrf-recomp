/* G42: the mirror's fixed-function vertex-state comparison, without the game.
 *
 * Texgen and fog colour are transcribed from the state at the draw; texture
 * transforms, lighting and fog from the inputs their updaters last emitted
 * with. Each must match the executor's latched registers word for word (or a
 * float within tolerance, counted apart); a wrong word lands in its register's
 * own counter; programmable draws compare fog only; a draw whose draw-time
 * state would transcribe differently is counted as a laziness disagreement
 * with the side the executor matched; SPECULAR_ENABLE follows whichever of
 * the light updater and the combiner builder wrote last; and the positive
 * control fails every group on every draw it compares. G42b's inverse
 * model-view (cases 11-18) follows the same pattern, compared only at draws
 * that light or use an eye-normal texgen, and never from a singular product. */
#include "d3d8_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int pgraph_d3d11_method(int subch, uint32_t m, uint32_t p) { (void)subch; (void)m; (void)p; return 1; }
void nv2a_pb_exec_method(uint32_t s, uint32_t m, uint32_t p) { (void)s; (void)m; (void)p; }

static D3D8HostDrawCheck c;
static D3D8ExecDrawTextures e;
static D3D8HostStats st, before;
static uint32_t W(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

static void lights_in(D3D8FFLightIn *in, int lit)
{
    memset(in, 0, sizeof *in);
    in->lighting = (uint32_t)lit; in->ambient = 0x00406080u; in->list_head = 1; in->nlights = 1;
    for (unsigned i = 0; i < 16; ++i) in->view[i] = W(i % 5 == 0 ? 1.0f : 0.0f);
    in->view[12] = W(3.0f);
    in->eye[2] = W(-1.0f);
    for (unsigned i = 0; i < 17; ++i) in->material[i] = W(0.5f + 0.01f * (float)i);
    in->light[0][0] = 3;
    for (unsigned i = 1; i < 12; ++i) in->light[0][i] = W(0.25f * (float)i);
    in->light[0][27] = W(0.6f); in->light[0][28] = W(0.0f); in->light[0][29] = W(-0.8f);
}
static void setup(void)
{
    memset(&c, 0, sizeof c);
    c.serial = 1; c.ffv_valid = 1;
    c.tss[0][D3D8FF_TSS_TEXCOORDINDEX] = 0x10000;         /* NORMAL_MAP on stage 0 */
    c.tss[1][D3D8FF_TSS_TEXCOORDINDEX] = 1;
    /* texture transforms: JSRF's stage-0 setup, COUNT2 from texgen. */
    c.tx_emit.ttf[0] = 2; c.tx_emit.tci[0] = 0x10000;
    for (unsigned i = 0; i < 16; ++i) c.tx_emit.matrix[0][i] = W((float)i * 0.5f);
    c.tx_cur = c.tx_emit; c.tx_emit_seen = 1; c.tx_emit_fresh = 1;
    lights_in(&c.lt_emit, 1);
    c.lt_cur = c.lt_emit; c.lt_emit_seen = 1; c.lt_emit_seq = 5;
    c.fg_emit.enable = 1; c.fg_emit.table_mode = 3; c.fg_emit.start = W(10.0f); c.fg_emit.end = W(300.0f);
    c.fg_emit.equal_scale = 0x46000000u;
    c.fg_cur = c.fg_emit; c.fg_emit_seen = 1;
    c.ffv_fog_color = 0x00112233u;
}
static void put_list(const D3D8FFReg *r, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) e.regs[r[i].method / 4u] = r[i].value;
}
/* The executor's registers as the transcription says D3D wrote them, from
 * the given inputs, with 0x3B8 optionally overridden (the builder wrote last). */
static void exec_from(const D3D8FFTexXformIn *tx, const D3D8FFLightIn *lt, const D3D8FFFogIn *fg, int sp)
{
    static D3D8FFLights lo;
    D3D8FFTexXform to;
    D3D8FFFog fo;
    memset(&e, 0, sizeof e);
    e.regs_valid = 1;
    for (unsigned s = 0; s < 4; ++s) {
        uint32_t m = d3d8_ff_texgen(c.tss[s][D3D8FF_TSS_TEXCOORDINDEX], NULL);
        e.regs[(0x3C0u + 0x10u * s) / 4u] = e.regs[(0x3C4u + 0x10u * s) / 4u] = e.regs[(0x3C8u + 0x10u * s) / 4u] = m;
    }
    d3d8_ff_tex_transforms(tx, &to);
    for (unsigned s = 0; s < 4; ++s) {
        e.regs[(0x420u + 4u * s) / 4u] = to.enable[s];
        if (to.matrix_written & (1u << s)) memcpy(&e.regs[(0x6C0u + 0x40u * s) / 4u], to.matrix[s], 64);
    }
    d3d8_ff_lights(lt, &lo);
    put_list(lo.reg, lo.n);
    if (sp >= 0) e.regs[0x3B8u / 4u] = (uint32_t)sp;
    d3d8_ff_fog(fg, &fo);
    e.regs[0x2A4u / 4u] = fo.enable;
    if (fo.params_written) {
        e.regs[0x2A0u / 4u] = fo.gen_mode; e.regs[0x29Cu / 4u] = fo.mode;
        for (unsigned k = 0; k < 3; ++k) e.regs[(0x9C0u + 4u * k) / 4u] = fo.params[k];
    }
    e.regs[0x2A8u / 4u] = d3d8_ff_fog_color(c.ffv_fog_color);
}
/* G42b: a lit draw with a WORLD*VIEW that inverts cleanly (WORLD a scale
 * and translation, VIEW a translation), last written in this draw's flush. */
static void imv_setup(void)
{
    static const float Wd[16] = { 2,0,0,0, 0,3,0,0, 0,0,.5f,0, 1,2,3,1 };
    static const float Vw[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,5,1 };
    setup();
    for (unsigned i = 0; i < 16; ++i) { c.imv_emit.world[i] = W(Wd[i]); c.imv_emit.view[i] = W(Vw[i]); }
    c.imv_emit.lighting = 1; c.imv_emit.dirty = 0x1200;
    c.imv_cur = c.imv_emit;
    c.imv_emit_seen = 1; c.imv_emit_fresh = 1; c.imv_emit_seq = c.imv_any_seq = 7;
}
/* The executor's MODELVIEW and INVERSE_MODELVIEW as the transcription says
 * 0x1962B0 wrote them from `in`. */
static void exec_imv(const D3D8FFInvMVIn *in, D3D8FFInvMV *o)
{
    d3d8_ff_inverse_modelview(in, o);
    for (unsigned k = 0; k < 16; ++k) e.regs[(0x480u + 4u * k) / 4u] = o->modelview[4u * (k % 4u) + k / 4u];
    for (unsigned k = 0; k < 12; ++k) e.regs[(0x580u + 4u * k) / 4u] = o->inverse[k];
}
#define D(f, g) (st.f[g] - before.f[g])
#define D1(f) (st.f - before.f)
static void run(void) { d3d8_host_get_stats(&before); d3d8_host_check_ff_vertex(&c, &e); d3d8_host_get_stats(&st); }

int main(void)
{
    enum { TG = D3D8_HOST_FFV_TEXGEN, TX = D3D8_HOST_FFV_TEXXFORM, LT = D3D8_HOST_FFV_LIGHT, FG = D3D8_HOST_FFV_FOG };

    /* 1. The executor holds exactly what the transcription says: all four match. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    d3d8_host_get_stats(&before);
    CHECK(d3d8_host_check_ff_vertex(&c, &e) == 1);
    d3d8_host_get_stats(&st);
    for (int g = 0; g < 4; ++g) { CHECK(D(ffv_draws, g) == 1); CHECK(D(ffv_all, g) == 1); }
    CHECK(D(ffv_words, TG) == 12 && D(ffv_exact, TG) == 12);
    CHECK(D(ffv_words, TX) == 4 + 16);                     /* 4 enables + stage 0's matrix */
    CHECK(D(ffv_words, FG) == 1 + 2 + 3 + 1);              /* enable, gen/mode, params, colour */
    CHECK(D(ffv_exact, LT) == D(ffv_words, LT) && D(ffv_words, LT) > 20);
    CHECK(st.tg_mode[0][4] - before.tg_mode[0][4] == 1);   /* stage 0 NORMAL_MAP */
    CHECK(st.tg_mode[1][0] - before.tg_mode[1][0] == 1);   /* stage 1 off */
    CHECK(D1(lt_lit) == 1 && st.lt_lights[1] - before.lt_lights[1] == 1 && D1(lt_dir) == 1);
    CHECK(D(tx_enabled, 0) == 1 && st.tx_case[4] - before.tx_case[4] == 1);   /* layout E */
    CHECK(D1(fg_on) == 1 && D(fg_table, 3) == 1 && D1(fg_color_nonzero) == 1);
    CHECK(D(ffv_lazy, LT) == 0 && D(ffv_fresh, TX) == 1);

    /* 2. One wrong matrix word: texture transforms fail, in that register's counter. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    e.regs[0x6C4u / 4u] ^= 0x00100000u;
    { unsigned long long r0 = d3d8_host_ffv_reg_mismatches(0x6C4);
      run();
      CHECK(D(ffv_all, TX) == 0 && D(ffv_draws, TX) == 1);
      CHECK(D(ffv_all, TG) == 1 && D(ffv_all, LT) == 1 && D(ffv_all, FG) == 1);
      CHECK(d3d8_host_ffv_reg_mismatches(0x6C4) == r0 + 1); }

    /* 3. A light float one ulp off: within tolerance, still a match, counted apart. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    e.regs[0x1034u / 4u] += 1u;                            /* INFINITE_DIRECTION x */
    run();
    CHECK(D(ffv_all, LT) == 1 && D(ffv_tol, LT) == 1);
    /* ... but a wrong control word is a mismatch. */
    e.regs[0x3BCu / 4u] = 3;
    run();
    CHECK(D(ffv_all, LT) == 0);

    /* 4. A programmable draw compares fog only. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    c.ffv_vs_flags = 0x10;
    run();
    CHECK(D(ffv_draws, TG) == 0 && D(ffv_draws, TX) == 0 && D(ffv_draws, LT) == 0);
    CHECK(D(ffv_draws, FG) == 1 && D(ffv_all, FG) == 1);

    /* 5. Laziness: lighting was switched off after the last emission and has
     * not been flushed; the executor holds the emission. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    c.lt_cur.lighting = 0;
    run();
    CHECK(D(ffv_all, LT) == 1 && D(ffv_lazy, LT) == 1 && D(ffv_emit_only, LT) == 1 && D(ffv_cur_only, LT) == 0);
    /* ... and if the executor held the draw-time state instead, the check fails
     * and the counter names the side. */
    exec_from(&c.tx_emit, &c.lt_cur, &c.fg_emit, -1);
    run();
    CHECK(D(ffv_all, LT) == 0 && D(ffv_cur_only, LT) == 1);

    /* 6. SPECULAR_ENABLE: the combiner builder wrote 0 after the light
     * updater's 1; the register must read 0, and reading 1 is a mismatch. */
    setup(); c.sp_emit_seq = 9; c.sp_emit_val = 0;
    exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, 0);
    run();
    CHECK(D(ffv_all, LT) == 1 && D1(lt_sp_from_builder) == 1);
    e.regs[0x3B8u / 4u] = 1;
    run();
    CHECK(D(ffv_all, LT) == 0);
    /* An older builder write does not count. */
    c.sp_emit_seq = 3;
    run();
    CHECK(D(ffv_all, LT) == 1 && D1(lt_sp_from_builder) == 0);

    /* 7. No emission seen yet: compared on the draw-time inputs, and counted. */
    setup(); c.tx_emit_seen = c.lt_emit_seen = c.fg_emit_seen = 0;
    memset(&c.tx_emit, 0, sizeof c.tx_emit);
    exec_from(&c.tx_cur, &c.lt_cur, &c.fg_cur, -1);
    run();
    CHECK(D(ffv_noemit, TX) == 1 && D(ffv_noemit, LT) == 1 && D(ffv_noemit, FG) == 1);
    CHECK(D(ffv_all, TX) == 1 && D(ffv_all, LT) == 1 && D(ffv_all, FG) == 1);

    /* 8. Fog off: only FOG_ENABLE and the colour are compared; stale mode and
     * params in the executor do not matter. */
    setup(); c.fg_emit.enable = 0; c.fg_cur = c.fg_emit;
    exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    e.regs[0x9C0u / 4u] = 0x12345678u;
    run();
    CHECK(D(ffv_all, FG) == 1 && D(ffv_words, FG) == 2);

    /* 9. Positive control: every group mismatches on a draw that matched. */
    setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    c.ffv_control = 1;
    d3d8_host_get_stats(&before);
    CHECK(d3d8_host_check_ff_vertex(&c, &e) == 0);
    d3d8_host_get_stats(&st);
    for (int g = 0; g < 4; ++g) { CHECK(D(ffv_draws, g) == 1); CHECK(D(ffv_all, g) == 0); }
    /* The census still counts the real mode, not the perturbed one. */
    CHECK(st.tg_mode[0][4] - before.tg_mode[0][4] == 1);
    /* And the programmable-draw control fails fog. */
    c.ffv_vs_flags = 0x10;
    run();
    CHECK(D(ffv_draws, FG) == 1 && D(ffv_all, FG) == 0);

    /* 10. Not filled by the mirror, or no executor registers: not compared. */
    setup(); c.ffv_valid = 0; exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
    run();
    for (int g = 0; g < 4; ++g) CHECK(D(ffv_draws, g) == 0);

    /* 11. The inverse model-view: the 12 words 0x190A30 wrote at the last
     * writing emission of 0x1962B0, at a draw that lights. */
    {
        enum { IV = D3D8_HOST_FFV_INVMV };
        D3D8FFInvMV io;
        imv_setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1); exec_imv(&c.imv_emit, &io);
        d3d8_host_get_stats(&before);
        CHECK(d3d8_host_check_ff_vertex(&c, &e) == 1);
        d3d8_host_get_stats(&st);
        CHECK(D(ffv_draws, IV) == 1 && D(ffv_all, IV) == 1);
        CHECK(D(ffv_words, IV) == 12 && D(ffv_exact, IV) == 12 && D(ffv_tol, IV) == 0);
        CHECK(D1(imv_needed) == 1 && D1(imv_singular) == 0 && D1(imv_normalize) == 0);
        CHECK(D1(imv_mv_draws) == 1 && D1(imv_mv_exact) == 1);   /* MODELVIEW from the same emission */
        CHECK(D(ffv_lazy, IV) == 0 && D(ffv_fresh, IV) == 1 && D(ffv_noemit, IV) == 0);
        /* the other four groups are unaffected */
        for (int g = 0; g < 4; ++g) CHECK(D(ffv_all, g) == 1);

        /* 12. One word wrong: a mismatch, in that register's counter; one ulp
         * off is within tolerance and counted apart. */
        { unsigned long long r0 = d3d8_host_ffv_reg_mismatches(0x5A8);
          e.regs[0x5A8u / 4u] ^= 0x00400000u;
          run();
          CHECK(D(ffv_all, IV) == 0 && d3d8_host_ffv_reg_mismatches(0x5A8) == r0 + 1);
          e.regs[0x5A8u / 4u] ^= 0x00400000u; e.regs[0x5A8u / 4u] += 1u;
          run();
          CHECK(D(ffv_all, IV) == 1 && D(ffv_tol, IV) == 1); }

        /* 13. Neither lighting nor an eye-normal texgen at the draw: 0x580 is
         * not used, and not compared, however stale. */
        imv_setup(); c.imv_cur.lighting = 0; exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
        run();
        CHECK(D(ffv_draws, IV) == 0 && D1(imv_needed) == 0);
        /* ... but an eye-normal texgen alone needs it. */
        c.imv_cur.eye_normal_mask = 1; exec_imv(&c.imv_emit, &io);
        run();
        CHECK(D(ffv_draws, IV) == 1 && D(ffv_all, IV) == 1);

        /* 14. A singular WORLD*VIEW: the guest sent 12 stale stack words.
         * Counted, not compared. */
        imv_setup(); c.imv_emit.world[0] = c.imv_emit.world[5] = c.imv_emit.world[10] = 0;
        c.imv_cur = c.imv_emit;
        exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1);
        run();
        CHECK(D1(imv_needed) == 1 && D1(imv_singular) == 1 && D(ffv_draws, IV) == 0);

        /* 15. Laziness: WORLD changed after the last writing emission. The
         * executor holds the emission, so the check passes and the counter
         * names the side; holding the draw-time state instead fails. */
        imv_setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1); exec_imv(&c.imv_emit, &io);
        c.imv_cur.world[0] = W(3.0f);
        run();
        CHECK(D(ffv_all, IV) == 1 && D(ffv_lazy, IV) == 1 && D(ffv_emit_only, IV) == 1);
        exec_imv(&c.imv_cur, &io);
        run();
        CHECK(D(ffv_all, IV) == 0 && D(ffv_cur_only, IV) == 1);

        /* 16. MODELVIEW last came from a later emission that did not write
         * 0x580 (lighting was off then): the 0x480 cross-check is skipped. */
        imv_setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1); exec_imv(&c.imv_emit, &io);
        c.imv_any_seq = c.imv_emit_seq + 1;
        run();
        CHECK(D(ffv_all, IV) == 1 && D1(imv_mv_draws) == 0);
        /* and a wrong MODELVIEW word from the same emission is seen. */
        c.imv_any_seq = c.imv_emit_seq; e.regs[0x484u / 4u] ^= 1u;
        run();
        CHECK(D1(imv_mv_draws) == 1 && D1(imv_mv_exact) == 0 && D(ffv_all, IV) == 1);

        /* 17. NORMALIZENORMALS on: the adjugate, counted. */
        imv_setup(); c.imv_emit.normalize = c.imv_cur.normalize = 1;
        exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1); exec_imv(&c.imv_emit, &io);
        run();
        CHECK(D(ffv_all, IV) == 1 && D1(imv_normalize) == 1);

        /* 18. Positive control: the inverse model-view fails too. */
        imv_setup(); exec_from(&c.tx_emit, &c.lt_emit, &c.fg_emit, -1); exec_imv(&c.imv_emit, &io);
        c.ffv_control = 1;
        run();
        for (int g = 0; g < 5; ++g) { CHECK(D(ffv_draws, g) == 1); CHECK(D(ffv_all, g) == 0); }
    }

    d3d8_host_report("test");
    printf("d3d8_ffv_check: all pass\n");
    return 0;
}
