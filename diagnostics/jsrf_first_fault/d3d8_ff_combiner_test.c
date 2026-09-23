/* G43: D3D's fixed-function combiner builder, transcribed.
 *
 * src/nv2a/d3d8_ff_combiner.c transcribes guest 0x00197F90, the XDK 4134
 * D3D8 routine that turns texture-stage states into NV2A register-combiner
 * words for every draw without a pixel shader (88% of JSRF's draws). Every
 * expected word below was derived BY HAND from the disassembly, not by
 * running the transcription:
 *
 *   input byte (helper 0x197EF0) = register | 0x10 (.a) | mapping << 5
 *     register: DIFFUSE 4 (V0), CURRENT 0xC (R0; V0 on the first stage),
 *               TEXTURE 8+stage (T0..T3), TFACTOR 1 (C0), SPECULAR 5 (V1),
 *               TEMP 0xD (R1)
 *     .a      : D3DTA_ALPHAREPLICATE, or any alpha pass
 *     mapping : 1 (0x20, INVERT) on D3DTA_COMPLEMENT, 2 (0x40) on DOT3
 *   slots: A bits 24..31, B 16..23, C 8..15, D 0..7
 *   output word: SUM_DST 0xC (R0) or 0xD (R1, RESULTARG = TEMP) at bits 8..11
 *
 * The positive control is the MODULATE vector: flip the texture from bound
 * to unbound and the word must change (it does -- NULL-TEXTURE below).
 */
#include "d3d8_ff_combiner.h"

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

enum { DIS = 1, SEL1, SEL2, MOD, MOD2X, MOD4X, ADD, ADDS, ADDS2X, SUB,
       ADDSMOOTH, BDA, BCA, BTA, BFA, BTAPM, PREMOD, MAAC, MCAA, MIAAC, MICAA,
       DOT3, MADD, LERP };
enum { DIF = 0, CUR = 1, TEX = 2, TFC = 3, SPC = 4, TMP = 5, CMP = 0x10, ARP = 0x20 };

static void stage(D3D8FFCombinerIn *in, int s,
                  unsigned cop, unsigned c0, unsigned c1, unsigned c2,
                  unsigned aop, unsigned a0, unsigned a1, unsigned a2, unsigned res)
{
    uint32_t *t = in->tss[s];
    t[12] = cop; t[13] = c0; t[14] = c1; t[15] = c2;
    t[16] = aop; t[17] = a0; t[18] = a1; t[19] = a2;
    t[20] = res;
}

static void init(D3D8FFCombinerIn *in)
{
    memset(in, 0, sizeof(*in));
    for (int s = 0; s < 4; s++)       /* D3D defaults past stage 0: DISABLE */
        stage(in, s, DIS, CUR, TEX, CUR, DIS, CUR, TEX, CUR, CUR);
}

static void expect_tail_zero(const D3D8FFCombiners *o, int from, const char *name)
{
    for (int k = from; k < 8; k++) {
        char w[80];
        snprintf(w, sizeof w, "%s slot %d all four", name, k);
        EQ(o->color_icw[k] | o->color_ocw[k] | o->alpha_icw[k] | o->alpha_ocw[k], 0, w);
    }
}

int main(void)
{
    D3D8FFCombinerIn in;
    D3D8FFCombiners o;

    /* MODULATE texture * diffuse, colour and alpha. */
    init(&in);
    stage(&in, 0, MOD, CUR, TEX, DIF, MOD, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    EQ(d3d8_ff_combiners(&in, &o), 0, "MODULATE rc");
    EQ(o.emitted, 1, "MODULATE emitted");
    EQ(o.combiner_control, 1, "MODULATE control");
    EQ(o.color_icw[0], 0x08040000, "MODULATE color_icw");   /* A=T0 B=V0 */
    EQ(o.color_ocw[0], 0x00000C00, "MODULATE color_ocw");   /* SUM->R0 */
    EQ(o.alpha_icw[0], 0x18140000, "MODULATE alpha_icw");   /* A=T0.a B=V0.a */
    EQ(o.alpha_ocw[0], 0x00000C00, "MODULATE alpha_ocw");
    EQ(o.specular_enable_emitted, 0, "MODULATE no specular");
    expect_tail_zero(&o, 1, "MODULATE");

    /* NULL-TEXTURE: same states, no texture bound on stage 0. The helper
     * returns 0xFFFFFFFF, and 0x19835E replaces the whole word with
     * "diffuse * 1" (first stage), or its .a form in the alpha pass. */
    in.texture_bound_mask = 0;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x04200000, "NULL-TEXTURE color_icw");
    EQ(o.alpha_icw[0], 0x14200000, "NULL-TEXTURE alpha_icw");
    EQ(o.color_ocw[0], 0x00000C00, "NULL-TEXTURE color_ocw kept");

    /* SELECTARG1 texture (A*1), SELECTARG2 diffuse (1*D in C*D). */
    init(&in);
    stage(&in, 0, SEL1, CUR, TEX, DIF, SEL2, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x08200000, "SELECTARG1 color_icw");
    EQ(o.alpha_icw[0], 0x00002014, "SELECTARG2 alpha_icw");  /* C=1 D=V0.a */

    /* DISABLE on stage 0: one stage passing diffuse colour and alpha,
     * ALPHAOP ignored. */
    init(&in);
    stage(&in, 0, DIS, CUR, TEX, DIF, SEL1, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.combiner_control, 1, "DISABLE0 control");
    EQ(o.color_icw[0], 0x04200000, "DISABLE0 color_icw");
    EQ(o.color_ocw[0], 0x00000C00, "DISABLE0 color_ocw");
    EQ(o.alpha_icw[0], 0x14200000, "DISABLE0 alpha_icw");
    EQ(o.alpha_ocw[0], 0x00000C00, "DISABLE0 alpha_ocw");
    expect_tail_zero(&o, 1, "DISABLE0");

    /* Two stages: stage 1 MODULATE2X texture * current, ALPHAOP DISABLE
     * (a later-stage alpha DISABLE writes 0/0); stage 2 DISABLE ends it. */
    init(&in);
    stage(&in, 0, MOD, CUR, TEX, DIF, MOD, CUR, TEX, DIF, CUR);
    stage(&in, 1, MOD2X, CUR, TEX, CUR, DIS, CUR, TEX, CUR, CUR);
    in.texture_bound_mask = 3;
    d3d8_ff_combiners(&in, &o);
    EQ(o.combiner_control, 2, "TWO control");
    EQ(o.color_icw[1], 0x090C0000, "TWO color_icw[1]");      /* A=T1 B=R0 */
    EQ(o.color_ocw[1], 0x00010C00, "TWO color_ocw[1]");      /* op 2: x2 */
    EQ(o.alpha_icw[1], 0, "TWO alpha_icw[1]");
    EQ(o.alpha_ocw[1], 0, "TWO alpha_ocw[1]");
    expect_tail_zero(&o, 2, "TWO");

    /* MODULATE4X, ADDSIGNED, ADDSIGNED2X: output scale/bias only. */
    init(&in);
    stage(&in, 0, MOD4X, CUR, TEX, DIF, ADDS, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_ocw[0], 0x00020C00, "MODULATE4X color_ocw");
    EQ(o.alpha_icw[0], 0x18202014, "ADDSIGNED alpha_icw");  /* T0.a*1 + 1*V0.a */
    EQ(o.alpha_ocw[0], 0x00008C00, "ADDSIGNED alpha_ocw");
    stage(&in, 0, ADDS2X, CUR, TEX, DIF, ADDS, CUR, TEX, DIF, CUR);
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_ocw[0], 0x00018C00, "ADDSIGNED2X color_ocw");

    /* BLENDTEXTUREALPHA colour; SELECTARG1 texture alpha. */
    init(&in);
    stage(&in, 0, BTA, CUR, TEX, DIF, SEL1, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x08183804, "BLENDTEXTUREALPHA color_icw"); /* T0*T0.a + (1-T0.a)*V0 */
    EQ(o.alpha_icw[0], 0x18200000, "SELECTARG1 alpha_icw");

    /* ADD with a complemented arg, SUBTRACT with SPECULAR in the alpha pass:
     * the specular flag goes 0 -> 1, so SET_SPECULAR_ENABLE(1) follows. */
    init(&in);
    stage(&in, 0, ADD, CUR, TEX | CMP, TFC, SUB, CUR, DIF, SPC, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x28202001, "ADD complement color_icw"); /* (1-T0)*1 + 1*C0 */
    EQ(o.alpha_icw[0], 0x14204015, "SUBTRACT alpha_icw");       /* V0.a*1 + (-1)*V1.a */
    EQ(o.device_flags, 0x40, "SPECULAR device flag");
    EQ(o.specular_enable_emitted, 1, "SPECULAR emitted");
    EQ(o.specular_enable, 1, "SPECULAR value");
    in.specular_enable = 1;                 /* title enabled it itself */
    d3d8_ff_combiners(&in, &o);
    EQ(o.specular_enable_emitted, 0, "SPECULAR rs=1 not emitted");
    /* Flag set from a previous draw, no specular now: emit 0. */
    init(&in);
    stage(&in, 0, SEL1, CUR, TEX, DIF, SEL1, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    in.device_flags = 0x41;
    d3d8_ff_combiners(&in, &o);
    EQ(o.device_flags, 0x01, "SPECULAR flag cleared, other bits kept");
    EQ(o.specular_enable_emitted, 1, "SPECULAR off emitted");
    EQ(o.specular_enable, 0, "SPECULAR off value");

    /* ALPHAREPLICATE in a colour arg. */
    init(&in);
    stage(&in, 0, SEL1, CUR, TEX | ARP, DIF, SEL1, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x18200000, "ALPHAREPLICATE color_icw");

    /* DOTPRODUCT3 into TEMP: AB dot with blue-to-alpha into R1; the alpha
     * half is zeroed and ALPHAOP is never read. */
    init(&in);
    stage(&in, 0, DOT3, CUR, TEX, DIF, MOD, CUR, TEX, DIF, TMP);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x48440000, "DOT3 color_icw");
    EQ(o.color_ocw[0], 0x000820D0, "DOT3 color_ocw");
    EQ(o.alpha_icw[0], 0, "DOT3 alpha_icw");
    EQ(o.alpha_ocw[0], 0, "DOT3 alpha_ocw");

    /* LERP colour, MULTIPLYADD alpha (the two ARG0 users). */
    init(&in);
    stage(&in, 0, LERP, TFC, TEX, DIF, MADD, DIF, TEX, TFC, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x01082104, "LERP color_icw");     /* C0*T0 + (1-C0)*V0 */
    EQ(o.alpha_icw[0], 0x14201811, "MULTIPLYADD alpha_icw"); /* V0.a*1 + T0.a*C0.a */

    /* MODULATEALPHA_ADDCOLOR / MODULATECOLOR_ADDALPHA. */
    init(&in);
    stage(&in, 0, MAAC, CUR, TEX, DIF, SEL1, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 1;
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x08201804, "MODULATEALPHA_ADDCOLOR color_icw"); /* T0 + T0.a*V0 */
    stage(&in, 0, MCAA, CUR, TEX, DIF, SEL1, CUR, TEX, DIF, CUR);
    d3d8_ff_combiners(&in, &o);
    EQ(o.color_icw[0], 0x08041820, "MODULATECOLOR_ADDALPHA color_icw"); /* T0*V0 + T0.a*1 */

    /* Point sprites: only stage 3, T3; stage 0 is not read at all (its
     * COLOROP 0 would be unresolvable if it were). */
    init(&in);
    in.tss[0][12] = 0;
    stage(&in, 3, MOD, CUR, TEX, DIF, MOD, CUR, TEX, DIF, CUR);
    in.texture_bound_mask = 8;
    in.point_sprite_enable = 1;
    EQ(d3d8_ff_combiners(&in, &o), 0, "POINTSPRITE rc");
    EQ(o.first_stage, 3, "POINTSPRITE first stage");
    EQ(o.combiner_control, 1, "POINTSPRITE control");
    EQ(o.color_icw[0], 0x0B040000, "POINTSPRITE color_icw"); /* A=T3 */
    EQ(o.alpha_icw[0], 0x1B140000, "POINTSPRITE alpha_icw");

    /* All four stages. */
    init(&in);
    for (int s = 0; s < 4; s++)
        stage(&in, s, MOD, CUR, TEX, CUR, SEL2, CUR, TEX, CUR, CUR);
    in.texture_bound_mask = 0xF;
    d3d8_ff_combiners(&in, &o);
    EQ(o.combiner_control, 4, "FOUR control");
    EQ(o.color_icw[0], 0x08040000, "FOUR color_icw[0]");  /* CURRENT = V0 on stage 0 */
    EQ(o.color_icw[3], 0x0B0C0000, "FOUR color_icw[3]");  /* T3 * R0 */
    EQ(o.alpha_icw[3], 0x0000201C, "FOUR alpha_icw[3]");  /* 1 * R0.a */
    expect_tail_zero(&o, 4, "FOUR");

    /* Unresolvable: COLOROP 0 jumps through 0x1984B0 in the guest. */
    init(&in);
    in.tss[0][12] = 0;
    EQ(d3d8_ff_combiners(&in, &o), (unsigned)-1, "BADOP rc");
    EQ(o.unresolved, D3D8FF_UNRES_BAD_OP, "BADOP flag");
    init(&in);
    stage(&in, 0, SEL1, CUR, 7, DIF, SEL1, CUR, TEX, DIF, CUR);
    EQ(d3d8_ff_combiners(&in, &o), (unsigned)-1, "BADARG rc");
    EQ(o.unresolved, D3D8FF_UNRES_BAD_ARG, "BADARG flag");

    /* A bound pixel shader: nothing. */
    init(&in);
    stage(&in, 0, MOD, CUR, TEX, DIF, MOD, CUR, TEX, DIF, CUR);
    in.pixel_shader = 0x1234;
    in.device_flags = 0x40;
    d3d8_ff_combiners(&in, &o);
    EQ(o.emitted, 0, "PS emitted");
    EQ(o.device_flags, 0x40, "PS device flags untouched");

    /* TEXTUREFACTOR (0x18ECC0) and the final combiner (0x195610). */
    uint32_t f0[8], f1[8], cw0 = 0, cw1 = 0;
    EQ(d3d8_ff_texture_factor(0x80FF4020u, 0, f0, f1), 1, "TFACTOR written");
    EQ(f0[0], 0x80FF4020u, "FACTOR0[0]");
    EQ(f1[7], 0x80FF4020u, "FACTOR1[7]");
    EQ(d3d8_ff_texture_factor(1, 1, f0, f1), 0, "TFACTOR with PS");
    d3d8_ff_final_combiner(1, 0, 0, 0, &cw0, &cw1);
    EQ(cw0, 0x130C0300, "FOG CW0");
    EQ(cw1, 0x00001C80, "FOG CW1");
    d3d8_ff_final_combiner(1, 1, 0, 0, &cw0, &cw1);
    EQ(cw0, 0x130E0300, "FOG+SPEC CW0");
    d3d8_ff_final_combiner(0, 0, 0, 0, &cw0, &cw1);
    EQ(cw0, 0x0000000C, "NOFOG CW0");
    d3d8_ff_final_combiner(0, 1, 0, 0, &cw0, &cw1);
    EQ(cw0, 0x0000000E, "NOFOG+SPEC CW0");
    EQ(d3d8_ff_final_combiner(0, 1, 1, 1, &cw0, &cw1), 0, "FINAL with PS");

    printf("d3d8_ff_combiner: %d checks, %s\n", checks, fail ? "FAILED" : "all pass");
    return fail;
}
