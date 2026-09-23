/* G43: the mirror's fixed-function combiner comparison, without the game.
 *
 * The transcription evaluated on the builder's LAST-EMITTED inputs must match
 * the executor's latched registers word for word; a wrong word lands in that
 * register's own counter and its group's; a pixel-shader draw is skipped; a
 * draw whose state at the draw would transcribe differently is counted as a
 * laziness disagreement, with which of the two the executor matched; the
 * positive control fails every fixed-function draw; and the discovery tally
 * counts distinct (inputs -> registers) pairings. */
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

/* Stage 0: colour MODULATE(TEXTURE, CURRENT), alpha SELECTARG1(TEXTURE),
 * texture bound; stage 1: DISABLE. D3D's common default chain. */
static void stage_words(D3D8FFCombinerIn *in, uint32_t colorop)
{
    memset(in, 0, sizeof *in);
    in->tss[0][D3D8FF_TSS_COLOROP] = colorop;
    in->tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_TEXTURE;
    in->tss[0][D3D8FF_TSS_COLORARG2] = D3D8FF_TA_CURRENT;
    in->tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1;
    in->tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_TEXTURE;
    in->tss[0][D3D8FF_TSS_ALPHAARG2] = D3D8FF_TA_CURRENT;
    in->tss[0][D3D8FF_TSS_RESULTARG] = D3D8FF_TA_CURRENT;
    for (unsigned s = 1; s < 4; ++s) { in->tss[s][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_DISABLE; in->tss[s][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_DISABLE; }
    in->texture_bound_mask = 1;
}
/* The executor's registers as the transcription says D3D wrote them. */
static void exec_from(const D3D8FFCombinerIn *in, uint32_t tfactor, const uint32_t fog[4])
{
    D3D8FFCombiners o;
    memset(&e, 0, sizeof e);
    e.ffc_valid = 1;
    CHECK(d3d8_ff_combiners(in, &o) == 0);
    e.ffc.w[0] = o.combiner_control;
    for (unsigned i = 0; i < 8; ++i) {
        e.ffc.w[1 + i] = o.color_icw[i]; e.ffc.w[9 + i] = o.color_ocw[i];
        e.ffc.w[17 + i] = o.alpha_icw[i]; e.ffc.w[25 + i] = o.alpha_ocw[i];
    }
    CHECK(d3d8_ff_texture_factor(tfactor, 0, &e.ffc.w[33], &e.ffc.w[41]) == 1);
    CHECK(d3d8_ff_final_combiner(fog[0], fog[1], fog[2], fog[3], &e.ffc.w[49], &e.ffc.w[50]) == 1);
}
static void setup(void)
{
    memset(&c, 0, sizeof c);
    c.serial = 1; c.ffc_valid = 1;
    stage_words(&c.ffc_cur, D3D8FF_TOP_MODULATE);
    c.ffc_emit = c.ffc_cur; c.ffc_emit_seen = 1; c.ffc_emit_fresh = 1;
    c.tfactor = 0x80FF8000u;
    c.fog_cur[0] = 1; c.fog_cur[1] = 0;
    memcpy(c.fog_emit, c.fog_cur, sizeof c.fog_emit); c.fog_emit_seen = 1;
    exec_from(&c.ffc_emit, c.tfactor, c.fog_emit);
}
#define DELTA(f) (st.f - before.f)
static int run(void)
{
    int r;
    d3d8_host_get_stats(&before);
    r = d3d8_host_check_combiners(&c, &e, NULL);
    d3d8_host_get_stats(&st);
    return r;
}

int main(void)
{
    char nm[32];
    /* The register table the executor reads and the report names. */
    CHECK(d3d8_host_ffc_method(0) == 0x1E60u && d3d8_host_ffc_method(1) == 0x0AC0u && d3d8_host_ffc_method(8) == 0x0ADCu);
    CHECK(d3d8_host_ffc_method(9) == 0x1E40u && d3d8_host_ffc_method(17) == 0x0260u && d3d8_host_ffc_method(25) == 0x0AA0u);
    CHECK(d3d8_host_ffc_method(33) == 0x0A60u && d3d8_host_ffc_method(41) == 0x0A80u && d3d8_host_ffc_method(48) == 0x0A9Cu);
    CHECK(d3d8_host_ffc_method(49) == 0x0288u && d3d8_host_ffc_method(50) == 0x028Cu);
    CHECK(!strcmp(d3d8_host_ffc_name(0, nm, sizeof nm), "COMBINER_CONTROL"));
    CHECK(!strcmp(d3d8_host_ffc_name(10, nm, sizeof nm), "COLOR_OCW[1]"));
    CHECK(!strcmp(d3d8_host_ffc_name(44, nm, sizeof nm), "FACTOR1[3]"));
    CHECK(!strcmp(d3d8_host_ffc_name(50, nm, sizeof nm), "SPECULAR_FOG_CW1"));

    /* Agreement, and the expected words written out by hand from the notes:
     * colour A = T0 (8), B = CURRENT on the first stage = V0 (4); alpha A =
     * T0.a (0x18), B = 1 (0x20); both outputs SUM_DST R0 (0xC00); one stage;
     * fog on without specular = lerp(fog.rgb, R0, fog.a). */
    setup();
    CHECK(e.ffc.w[0] == 1u && e.ffc.w[1] == 0x08040000u && e.ffc.w[9] == 0x00000C00u);
    CHECK(e.ffc.w[17] == 0x18200000u && e.ffc.w[25] == 0x00000C00u && e.ffc.w[2] == 0u);
    CHECK(e.ffc.w[33] == 0x80FF8000u && e.ffc.w[48] == 0x80FF8000u);
    CHECK(e.ffc.w[49] == 0x130C0300u && e.ffc.w[50] == 0x1C80u);
    CHECK(run() == 1);
    CHECK(DELTA(ffc_draws) == 1 && DELTA(ffc_all_match) == 1 && DELTA(ffc_builder_match) == 1);
    CHECK(DELTA(ffc_factor_match) == 1 && DELTA(ffc_final_match) == 1 && DELTA(ffc_fresh) == 1);
    CHECK(DELTA(ffc_lazy_differs) == 0 && DELTA(ffc_no_emit) == 0 && DELTA(ffc_unresolved) == 0);

    /* One wrong builder word: its own counter, the builder group, nothing else. */
    setup(); e.ffc.w[9] ^= 0x10000u;                 /* COLOR_OCW[0] scale x2 */
    CHECK(run() == 0);
    CHECK(DELTA(ffc_word_mismatch[9]) == 1 && DELTA(ffc_builder_match) == 0);
    CHECK(DELTA(ffc_factor_match) == 1 && DELTA(ffc_final_match) == 1 && DELTA(ffc_all_match) == 0);

    /* A wrong factor word and a wrong final-combiner word. */
    setup(); e.ffc.w[44] = 0;                        /* FACTOR1[3] */
    CHECK(run() == 0);
    CHECK(DELTA(ffc_word_mismatch[44]) == 1 && DELTA(ffc_factor_match) == 0 && DELTA(ffc_builder_match) == 1);
    setup(); c.fog_emit[1] = 1;                      /* D3D had SPECULARENABLE on at the fog update */
    CHECK(run() == 0);
    CHECK(DELTA(ffc_word_mismatch[49]) == 1 && DELTA(ffc_final_match) == 0 && DELTA(ffc_builder_match) == 1);

    /* A pixel shader bound at the draw: not a fixed-function draw at all. */
    setup(); c.ffc_ps = 0x80123400u; e.ffc.w[1] = 0xDEADBEEFu;
    CHECK(run() == 1);
    CHECK(DELTA(ffc_ps_skipped) == 1 && DELTA(ffc_draws) == 0);

    /* Laziness. The builder last ran on MODULATE; the state at the draw says
     * ADD. The executor holds what was emitted: a match, counted as a
     * disagreement the last emission explains. */
    setup(); stage_words(&c.ffc_cur, D3D8FF_TOP_ADD); c.ffc_emit_fresh = 0;
    CHECK(run() == 1);
    CHECK(DELTA(ffc_lazy_differs) == 1 && DELTA(ffc_exec_emit_only) == 1 && DELTA(ffc_exec_cur_only) == 0);
    CHECK(DELTA(ffc_fresh) == 0);
    /* ... and the other way: the executor holds the draw-time state's words,
     * so the check (which follows the emission) fails and says why. */
    setup(); stage_words(&c.ffc_cur, D3D8FF_TOP_ADD); exec_from(&c.ffc_cur, c.tfactor, c.fog_emit);
    CHECK(run() == 0);
    CHECK(DELTA(ffc_lazy_differs) == 1 && DELTA(ffc_exec_cur_only) == 1 && DELTA(ffc_exec_emit_only) == 0);

    /* No emission seen yet: the draw-time state stands in, and is counted. */
    setup(); c.ffc_emit_seen = 0; memset(&c.ffc_emit, 0, sizeof c.ffc_emit);
    CHECK(run() == 1);
    CHECK(DELTA(ffc_no_emit) == 1 && DELTA(ffc_all_match) == 1);

    /* The fog updater skipped CW0/CW1 (pixel shader with a final combiner):
     * not compared, counted. */
    setup(); c.fog_emit[2] = 1; c.fog_emit[3] = 1; e.ffc.w[49] = 0x12345678u;
    CHECK(run() == 1);
    CHECK(DELTA(ffc_final_skipped) == 1 && DELTA(ffc_word_mismatch[49]) == 0 && DELTA(ffc_all_match) == 0);

    /* A COLOROP the guest would jump through garbage on: builder words skipped. */
    setup(); c.ffc_emit.tss[0][D3D8FF_TSS_COLOROP] = 0;
    CHECK(run() == 1);
    CHECK(DELTA(ffc_unresolved) == 1 && DELTA(ffc_builder_match) == 0 && DELTA(ffc_factor_match) == 1);

    /* The positive control: an agreeing draw must fail, on COLOR_ICW[0]. */
    setup(); c.ffc_control = 1;
    {   D3D8CombinerRegs x;
        d3d8_host_get_stats(&before);
        CHECK(d3d8_host_check_combiners(&c, &e, &x) == 0);
        d3d8_host_get_stats(&st);
        CHECK(x.w[1] == (e.ffc.w[1] ^ 1u));
        CHECK(DELTA(ffc_word_mismatch[1]) == 1 && DELTA(ffc_all_match) == 0); }

    /* Discovery tally: the same setup twice is one pairing; another TFACTOR
     * is another. */
    {   unsigned p0;
        setup(); c.tfactor = 0x11223344u; exec_from(&c.ffc_emit, c.tfactor, c.fog_emit);
        d3d8_host_get_stats(&st); p0 = st.ffc_tally_pairs;
        CHECK(run() == 1); CHECK(st.ffc_tally_pairs == p0 + 1);
        CHECK(run() == 1); CHECK(st.ffc_tally_pairs == p0 + 1);
        c.tfactor = 0x55667788u; exec_from(&c.ffc_emit, c.tfactor, c.fog_emit);
        CHECK(run() == 1); CHECK(st.ffc_tally_pairs == p0 + 2);
        /* Words of stages after the chain ended do not split a pairing. */
        c.ffc_emit.tss[2][D3D8FF_TSS_COLORARG1] = 7;
        CHECK(run() == 1); CHECK(st.ffc_tally_pairs == p0 + 2); }
    d3d8_host_ffc_tally_report("test", 5);
    d3d8_host_report("test");
    fprintf(stderr, "d3d8_combiner_check_test: all checks passed\n");
    return 0;
}
