/* NV2A field/combiner semantics: nv2a_regs.h and xemu pgraph/texture.c,
 * pgraph/glsl/psh.c. This implements the measured copy program, not a general
 * register-combiner interpreter. Keep unsupported states explicit. */
#include "nv2a_texture_copy.h"
#include "nv2a_vsh.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../recomp_switch.h"   /* RECOMP_TEXMODE_APPROX */
#include "../d3d/d3d8_combiner_bits.h"
#include "nv2a_drop.h"

#define M(a) m[(a)/4]

static unsigned long s_fmt_seen[256], s_fmt_rejected[256];
static unsigned long s_reject_hdr, s_reject_dma, s_reject_combiner_out;
static unsigned long s_dma_rejected[4];

/* WHICH combiner state is refused, not merely that one was.
 *
 * The texture census settled the format question in one run by counting the
 * INPUT rather than the gate. The combiner gate has nine distinct refusals
 * all reported through four strings, and the graffiti pixel shader
 * (def 0x0020C0B8, the title's only 8-stage program) is known statically to
 * trip at least three of them -- output words 0x000820D0 carrying
 * AB_DOT_PRODUCT and AB_BLUE_TO_ALPHA, outputs routed to texture registers
 * T0-T3, and nonzero per-stage C0. Four other shaders share the constant
 * case. Counting says which of those actually fires, and how much of the
 * frame each one costs. */
static unsigned long s_rej_final, s_rej_control, s_rej_outreg, s_rej_inreg;
static unsigned long s_rej_const, s_rej_texmode, s_rej_texstage, s_rej_alpha;
/* WHICH texture shader mode, and on which unit. "texture shader mode" is the
 * last refusal left in this gate after the graffiti work, and it names the
 * gate rather than the value -- the same blind spot the format census fixed
 * in one run. 179 refused draws in the player's 21 Sep session is a small,
 * bursty number: the shape of an effect that only appears sometimes, which is
 * what "boost flickers" would look like from in here. */
static unsigned long s_texmode_seen[32][4];
static unsigned long s_texmode_approx[32][4];

/* RECOMP_TEXMODE_APPROX: draw unimplemented bump modes flat rather than
 * dropping the draw. Read once; see the note at the gate.
 *
 * THROUGH THE HELPER, not `getenv(...) != NULL`. This switch changes what
 * reaches the screen, and the A/B the handover asked for -- "run with
 * RECOMP_TEXMODE_APPROX=0; if the flicker persists it is not mine" -- is
 * exactly the arm a presence test cannot express: =0 would have turned the
 * approximation ON and reported the two arms as agreeing. recomp_switch.h
 * records three earlier conclusions lost to that reading. */
static int texmode_approx_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_TEXMODE_APPROX");
    return on;
}
/* RECOMP_TEXMODE_BUMP, default ON: texture shader modes 6 and 7 are drawn
 * with their displacement (see NV2ATextureCopy.bump). =0 puts back exactly
 * the behaviour before it: approximated as plain 2D under
 * RECOMP_TEXMODE_APPROX, refused without it -- which is the A/B arm. */
static int texmode_bump_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on_default("RECOMP_TEXMODE_BUMP", 1);
    return on;
}
static unsigned long s_texmode_bump[32][4];
static float f32_bits(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static unsigned long s_rej_count_hist[10];
#define VR_OCW_SLOTS 16
static uint32_t s_bad_ocw[VR_OCW_SLOTS]; static unsigned long s_bad_ocw_n[VR_OCW_SLOTS];
static unsigned s_bad_ocw_used;

/* Every distinct nonzero per-stage constant seen on a refused draw. Item 3 of
 * the graffiti blocker list cannot be counted by s_rej_const, because the
 * output-mode check returns first and that counter never increments. */
static uint32_t s_bad_c0[VR_OCW_SLOTS]; static unsigned s_bad_c0_used;
static unsigned long s_bad_dst_reg[16];

static void note_bad_c0(uint32_t v)
{
    unsigned i;
    if (!v) return;
    for (i = 0; i < s_bad_c0_used; ++i) if (s_bad_c0[i] == v) return;
    if (s_bad_c0_used < VR_OCW_SLOTS) s_bad_c0[s_bad_c0_used++] = v;
}

static void note_bad_ocw(uint32_t w)
{
    for (unsigned i=0;i<s_bad_ocw_used;++i)
        if (s_bad_ocw[i]==w) { s_bad_ocw_n[i]++; return; }
    if (s_bad_ocw_used<VR_OCW_SLOTS) {
        s_bad_ocw[s_bad_ocw_used]=w; s_bad_ocw_n[s_bad_ocw_used]=1; s_bad_ocw_used++;
    }
}

/* ---- G53: the final combiner and fog ---------------------------------- */
/* RECOMP_FOG, default ON: draw fogged draws with the final combiner in full.
 * =0 is the old gate, which refuses (drops) every final combiner but the two
 * fog-off ones. RECOMP_FOG_IGNORE=1 is the positive control: D3D's fog
 * programs are drawn as their fog-off twins, so the geometry appears unfogged
 * -- it proves the refused draws are the missing ones, and nothing else. */
static int fog_model_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on_default("RECOMP_FOG",1); return on; }
static int fog_ignore_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_FOG_IGNORE"); return on; }
#define FOG_WORDS 32
static struct { uint32_t cw0, cw1; const char *why; unsigned long n; } s_final_refused[FOG_WORDS];
static unsigned s_final_refused_n; static unsigned long s_final_refused_overflow;
static unsigned long s_final_general, s_final_ignored, s_fog_modes[8], s_fog_mode_other;
static void note_final_refused(uint32_t w0, uint32_t w1, const char *why)
{
    for (unsigned i = 0; i < s_final_refused_n; ++i)
        if (s_final_refused[i].cw0 == w0 && s_final_refused[i].cw1 == w1) { ++s_final_refused[i].n; return; }
    if (s_final_refused_n >= FOG_WORDS) { ++s_final_refused_overflow; return; }
    s_final_refused[s_final_refused_n].cw0 = w0; s_final_refused[s_final_refused_n].cw1 = w1;
    s_final_refused[s_final_refused_n].why = why; s_final_refused[s_final_refused_n++].n = 1;
    /* LOUD, once per program: a refused final combiner is a dropped draw. */
    fprintf(stderr, "[FOG] final combiner CW0 %08X CW1 %08X refused (%s): every draw using it is DROPPED\n",
            w0, w1, why);
}
float nv2a_fog_factor(uint32_t mode, float p0, float p1, float d)
{
    float f;
    if (mode == 0x804 || mode == 0x802 || mode == 0x803) d = fabsf(d);
    switch (mode) {
    case 0x2601: case 0x804:
        if (!isfinite(d)) d = 0;
        f = p0 + d * p1 - 1.0f; break;
    case 0x800: case 0x802:
        if (!isfinite(d)) d = 0;
        f = p0 + exp2f(d * p1 * 16.0f) - 1.5f; break;
    case 0x801: case 0x803:
        f = p0 + exp2f(-d * d * p1 * p1 * 32.0f) - 1.5f; break;
    default: return 1.0f;
    }
    return f < 0 ? 0 : f > 1 ? 1 : f;   /* NaN stays NaN, then clamps to 0 below */
}
static int final_input_ok(uint32_t code)
{
    unsigned reg = code & 15;
    if (code & 0xC0u) return 0;               /* final inputs are unsigned: identity or invert only */
    return reg != 6 && reg != 7;              /* 6, 7 name nothing */
}
const char *nv2a_final_combiner_supported(uint32_t cw0, uint32_t cw1)
{
    for (unsigned k = 0; k < 4; ++k) if (!final_input_ok((cw0 >> (8 * k)) & 255u)) return "final combiner input";
    for (unsigned k = 1; k < 4; ++k) if (!final_input_ok((cw1 >> (8 * k)) & 255u)) return "final combiner input";
    if (cw1 & 0x1Fu) return "final combiner setting bits";
    /* E and F feed EF_PROD, which cannot read itself or the sum. */
    for (unsigned k = 2; k < 4; ++k) { unsigned r = (cw1 >> (8 * k)) & 15u; if (r == 14 || r == 15) return "final combiner E/F source"; }
    return NULL;
}
static float fin_in(uint32_t code, unsigned ch, float regs[16][4])
{
    float x = regs[code & 15][(code & 16) ? 3 : ch];
    return (code & 32) ? 1 - fminf(1, fmaxf(0, x)) : fmaxf(0, x);
}
static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 255) / 255.0f; o[1] = (float)((c >> 8) & 255) / 255.0f;
    o[2] = (float)(c & 255) / 255.0f; o[3] = (float)((c >> 24) & 255) / 255.0f;
}
void nv2a_final_combine(const NV2ATextureCopy *s, float regs[16][4], float fog, float out[4])
{
    uint32_t w0 = s->final_cw0, w1 = s->final_cw1;
    unpack_argb(s->spec_fog_c0, regs[1]); unpack_argb(s->spec_fog_c1, regs[2]);
    regs[3][0] = (float)(s->fog_color & 255) / 255.0f; regs[3][1] = (float)((s->fog_color >> 8) & 255) / 255.0f;
    regs[3][2] = (float)((s->fog_color >> 16) & 255) / 255.0f; regs[3][3] = fog;
    for (unsigned k = 0; k < 3; ++k) {
        float v1 = regs[5][k], r0 = regs[12][k];
        if (w1 & 0x40) v1 = 1 - fminf(1, fmaxf(0, v1));
        if (w1 & 0x20) r0 = 1 - fminf(1, fmaxf(0, r0));
        regs[14][k] = v1 + r0;
        if (w1 & 0x80) regs[14][k] = fminf(1, fmaxf(0, regs[14][k]));
    }
    regs[14][3] = 0;
    for (unsigned k = 0; k < 3; ++k) regs[15][k] = fin_in(w1 >> 24, k, regs) * fin_in((w1 >> 16) & 255, k, regs);
    regs[15][3] = 0;
    for (unsigned k = 0; k < 3; ++k) {
        float a = fin_in(w0 >> 24, k, regs), b = fin_in((w0 >> 16) & 255, k, regs);
        float c = fin_in((w0 >> 8) & 255, k, regs), d = fin_in(w0 & 255, k, regs);
        out[k] = fminf(1, fmaxf(0, d + a * b + (1 - a) * c));
    }
    out[3] = fminf(1, fmaxf(0, fin_in((w1 >> 8) & 255, 2, regs)));
}
void nv2a_fog_census(void)
{
    fprintf(stderr, "[FOG] final combiner: general programs drawn %lu, fog programs drawn unfogged (RECOMP_FOG_IGNORE) %lu,"
                    " RECOMP_FOG=%s | fog modes: LINEAR %lu EXP %lu EXP2 %lu LINEAR_ABS %lu EXP_ABS %lu EXP2_ABS %lu other %lu\n",
            s_final_general, s_final_ignored, fog_model_on() ? "on" : "OFF (refused, dropped)",
            s_fog_modes[0], s_fog_modes[1], s_fog_modes[2], s_fog_modes[3], s_fog_modes[4], s_fog_modes[5], s_fog_mode_other);
    for (unsigned i = 0; i < s_final_refused_n; ++i)
        fprintf(stderr, "[FOG]   DROPPED %lu draws: final combiner CW0 %08X CW1 %08X (%s)\n",
                s_final_refused[i].n, s_final_refused[i].cw0, s_final_refused[i].cw1, s_final_refused[i].why);
    if (s_final_refused_overflow) fprintf(stderr, "[FOG]   DROPPED %lu more draws with other programs\n", s_final_refused_overflow);
}

const char *nv2a_texture_copy_prepare(const uint32_t m[2048], NV2ATextureCopy *s)
{
    uint32_t fw0 = M(0x288), fw1 = M(0x28c);
    int fog_off_program = (fw0==0xc || fw0==0xe) && fw1==0x1c80;
    memset(s, 0, sizeof(*s));
    if (!fog_off_program) {
        const char *why = fog_model_on() ? nv2a_final_combiner_supported(fw0, fw1) : "RECOMP_FOG=0";
        if (!why && fog_ignore_on() && (fw0==0x130C0300u || fw0==0x130E0300u) && fw1==0x1c80) {
            /* The control: D3D's fog program drawn as its fog-off twin. */
            fw0 = fw0==0x130E0300u ? 0xeu : 0xcu; fog_off_program = 1; ++s_final_ignored;
            nv2a_drop(NV2A_DROP_SIMPLIFIED, "fragment", "fog program drawn unfogged (RECOMP_FOG_IGNORE)", fw0);
        } else if (why) {
            s_rej_final++; note_final_refused(fw0, fw1, why);
            return "combiner / texture program";
        }
    }
    if (!fog_off_program) {
        uint32_t fm = M(0x29c);
        s->final_general = 1; s->final_cw0 = fw0; s->final_cw1 = fw1; ++s_final_general;
        switch (fm) { case 0x2601: ++s_fog_modes[0]; break; case 0x800: ++s_fog_modes[1]; break;
                      case 0x801: ++s_fog_modes[2]; break; case 0x804: ++s_fog_modes[3]; break;
                      case 0x802: ++s_fog_modes[4]; break; case 0x803: ++s_fog_modes[5]; break;
                      default: ++s_fog_mode_other; break; }
    }
    s->fog_enable = M(0x2a4) != 0; s->fog_mode = M(0x29c); s->fog_color = M(0x2a8);
    memcpy(&s->fog_p0, &M(0x9c0), 4); memcpy(&s->fog_p1, &M(0x9c4), 4);
    s->spec_fog_c0 = M(0x1e20); s->spec_fog_c1 = M(0x1e24);
    s->point_size = (float)(M(0x43c) & 0x1ffu) / 8.0f; s->point_sprite = M(0x31c) != 0; s->point_params = M(0x318) != 0;
    uint32_t control=M(0x1e60),count=control&0xf;
    /* Count occupies the low nibble. The three high flags select mux and
     * per-stage C0/C1; the captured six-stage program sets all three. */
    if (count<1 || count>8 || (control&~0x0001110fu)) {
        s_rej_control++;
        if (count<10) s_rej_count_hist[count]++;
        return "combiner / texture program";
    }
    s->combiner_count=count; s->add_specular=!s->final_general && fw0==0xe;
    /* The measured programs use plain AB, CD, or AB+CD routing to R0/R1.
     * Keep dot, mux, bias and scale modes explicitly unsupported. */
    for (unsigned i=0;i<s->combiner_count;++i) {
        unsigned uses_constant=0;
        s->color_ocw[i]=M(0x1e40+4*i);s->alpha_ocw[i]=M(0xaa0+4*i);
        uint32_t outputs[2]={s->color_ocw[i],s->alpha_ocw[i]};
        for(unsigned j=0;j<2;++j) {
            /* WHICH BITS THIS RUNTIME MODELS, from xemu's parse_combiner_output
             * -- the reference this file's header already names:
             *
             *    0..3   CD destination      12  CD dot product
             *    4..7   AB destination      13  AB dot product
             *    8..11  SUM destination     14  mux instead of sum
             *    15..17 output mapping      18  CD blue-to-alpha
             *                               19  AB blue-to-alpha
             *
             * Everything above bit 19 is still unmodelled and still refused,
             * so a program using something we have never seen is dropped
             * rather than shaded wrong.
             *
             * THE BIT LAYOUT WAS WORTH CHECKING. `d3d8_combiners.c:169` reads
             * bits 0..3 as the AB destination and 4..7 as CD -- the opposite
             * way round -- so the two decoders in this tree disagreed, and the
             * D3D11 path is the one that is inverted. That is a separate bug;
             * this file was already right. And "bit 19 is AB blue-to-alpha"
             * appeared in a handover with no source anywhere, citing only
             * itself, until it was checked against xemu. */
            if(outputs[j]&~0xfffffu) {
                unsigned st, sh, k;
                for (st = 0; st < s->combiner_count; ++st) {
                    uint32_t ow[2] = { M(0x1e40+4*st), M(0xaa0+4*st) };
                    for (k = 0; k < 2; ++k)
                        for (sh = 0; sh < 12; sh += 4)
                            s_bad_dst_reg[(ow[k] >> sh) & 15]++;
                }
                for (st = 0; st < 8; ++st) {
                    note_bad_c0(M(0xa60+4*st));
                    note_bad_c0(M(0xa80+4*st));
                }
                s_reject_combiner_out++; note_bad_ocw(outputs[j]);
                return "combiner output mode";
            }
            /* Destinations 8..11 are the texture registers T0..T3, and the
             * register file has always had room for them -- only the gate
             * stood in front. The graffiti shader writes T0 and T2 and reads
             * them back in a later stage, which is why refusing them dropped
             * the whole program.
             *
             * WHAT IS *NOT* WIDENED, and the existing suite is why. The first
             * version of this accepted anything <= 13, which let a stage write
             * the CONSTANT registers 1 and 2 -- they are inputs, a write to
             * them is meaningless, and jsrf_texture_copy has asserted since
             * long before today that 0xc01 must be refused. It failed, which
             * is the test doing its job. 0 is discard, 8..13 are T0..T3/R0/R1;
             * 1..7 and 14..15 stay refused and will show up in the census if a
             * program ever asks for one. */
            for(unsigned shift=0;shift<12;shift+=4) {
                unsigned dst=(outputs[j]>>shift)&15;
                if(dst && (dst<8 || dst>13)) {
                    s_rej_outreg++; note_bad_ocw(outputs[j]);
                    return "combiner output register";
                }
            }
        }
        s->color_icw[i]=M(0xac0+4*i); s->alpha_icw[i]=M(0x260+4*i);
        for (unsigned j=0;j<4;++j) {
            unsigned regs[2]={(s->color_icw[i]>>(8*j))&15,(s->alpha_icw[i]>>(8*j))&15};
            for(unsigned k=0;k<2;++k) {
                if (regs[k]!=0 && regs[k]!=1 && regs[k]!=2 && regs[k]!=4 && regs[k]!=5 &&
                        !(regs[k]>=8 && regs[k]<=13)) {
                    s_rej_inreg++; return "combiner input register";
                }
                uses_constant|=regs[k]==1 || regs[k]==2;
            }
        }
        /* Per-stage constants are now LOADED rather than refused. They used
         * to be dropped on the reasoning that the one captured program set
         * them all to zero, so representing them could wait; the graffiti
         * shader sets three of them to channel masks and they carry the
         * effect. `uses_constant` is no longer a reason to refuse, only a
         * note that this stage reads reg 1 or 2. */
        (void)uses_constant;
        s->const0[i]=M(0xa60+4*i); s->const1[i]=M(0xa80+4*i);
    }
    for (unsigned u=0;u<4;++u) {
        unsigned mode=(M(0x1e70)>>(5*u))&31;
        /* REFUSING A DRAW IS THE WORST AVAILABLE FALLBACK, and for BUMPENVMAP
         * it is what we have been doing.
         *
         * Modes 6 and 7 (BUMPENVMAP, BUMPENVMAP_LUMINANCE -- xemu psh.c)
         * sample this unit's texture at coordinates perturbed by the previous
         * stage's output. We implement neither, and an unimplemented mode
         * drops the ENTIRE draw: not a flat effect, a hole where the geometry
         * should be. A player reported exactly that on 21 Sep 2026 -- "almost
         * black and purple transition between title card/intro/corn" -- in a
         * session whose only renderer refusal was 179 draws, unit 1 mode 6,
         * in one ten-second burst.
         *
         * Approximating the perturbation as ZERO makes it a plain 2D fetch.
         * The distortion is lost and the surface is drawn, which is strictly
         * closer to the frame than nothing at all. It is an approximation and
         * it is named as one, so the census below still reports what was
         * approximated rather than pretending the mode is implemented.
         *
         * Off by default like every other behaviour switch here: it changes
         * what reaches the screen and has to be measurable against its own
         * absence.
         *
         * IMPLEMENTED SINCE 24 SEP 2026, under RECOMP_TEXMODE_BUMP (default
         * on), for units 1..3 whose input unit is an earlier one -- xemu
         * asserts the same (stage >= 1). Unit 1 always reads unit 0; units 2
         * and 3 read the unit named in NV097_SET_SHADER_OTHER_STAGE_INPUT
         * (0x1E78) bits 16..19 and 20..23. What falls outside that is still
         * approximated or refused below, and bump_approx still means exactly
         * "drawn WITHOUT its displacement" -- RECOMP_MARK_BUMP keeps marking
         * only those. The implemented ones are marked by RECOMP_MARK_BUMP_ENV. */
        if ((mode==6 || mode==7) && u>=1 && texmode_bump_on()) {
            unsigned in = u==1 ? 0u : (M(0x1e78) >> (16 + 4*(u-2))) & 15u;
            if (in < u) {
                /* The method words are M00, M01, M11, M10: the XDK's
                 * SetTextureState_BumpEnv (0x0018F180 in this title) writes
                 * D3DTSS type t to 0x1AD0 + 64*stage + 4*t, and the Xbox
                 * numbering is 00=22, 01=23, 11=24, 10=25 -- which is also
                 * xemu's swizzle {00, 01, 11, 10} in SET_TEXTURE_SET_BUMP_ENV_MAT. */
                unsigned b = 0x1b28 + 64*u;
                s->bump[u] = mode; s->bump_input[u] = in;
                s->bump_mat[u][0] = f32_bits(M(b));       /* M00 */
                s->bump_mat[u][1] = f32_bits(M(b + 4));   /* M01 */
                s->bump_mat[u][2] = f32_bits(M(b + 12));  /* M10 */
                s->bump_mat[u][3] = f32_bits(M(b + 8));   /* M11 */
                s->bump_scale[u] = f32_bits(M(0x1b38 + 64*u));
                s->bump_offset[u] = f32_bits(M(0x1b3c + 64*u));
                s_texmode_bump[mode][u]++;
                mode = 1;
            }
        }
        if ((mode==6 || mode==7) && texmode_approx_on()) {
            s_texmode_approx[mode][u]++;
            s->bump_approx=1;
            nv2a_drop(NV2A_DROP_SIMPLIFIED, "fragment", "bump-map texture mode drawn without its displacement",
                      mode | (u << 8));
            mode=1;
        }
        if (mode>1) {
            unsigned m2, u2;
            /* Record the whole program's modes, not just the one that tripped:
             * a unit refused for mode 3 while another wants mode 2 is two
             * features, and the early return hides the second. */
            for (u2 = 0; u2 < 4; ++u2) {
                m2 = (M(0x1e70) >> (5*u2)) & 31;
                s_texmode_seen[m2][u2]++;
            }
            s_rej_texmode++; return "texture shader mode";
        }
        if (mode) {
            if (!(M(0x1b0c+64*u)&0x40000000)) {
                s_rej_texstage++; return "disabled texture shader stage";
            }
            s->texture_mask|=1u<<u;
        }
    }
    s->untextured=!(s->texture_mask&1); s->modulate=1;
    /* Retain the proven exact framebuffer-copy fast path. */
    if(s->texture_mask==1 && s->combiner_count==1 && !s->add_specular &&
            s->color_icw[0]==0x08200000 && s->alpha_icw[0]==0x14200000) {
        s->combiner_count=0; s->modulate=0;
    }
    if (M(0x300) && (M(0x300)!=1 || M(0x33c)!=0x204 || M(0x340)>255)) {
        s_rej_alpha++; return "alpha test";
    }
    s->alpha_test=M(0x300); s->alpha_ref=M(0x340);
    /* Depth range and policy, straight from the guest rather than assumed.
     * ZCLAMP_EN is a FOUR-BIT field at 0xF0, not bit 0 -- JSRF writes 1,
     * which is ZCLAMP_EN_CULL (0), i.e. discard outside the range. Reading
     * it as bit 0 gives CLAMP and the opposite behaviour. */
    { union { uint32_t u; float f; } lo, hi;
      lo.u = M(0x394); hi.u = M(0x398);
      /* An unwritten method reads 0 here, and 0 is a legitimate minimum but
       * not a legitimate maximum -- fall back to the Z24 full range rather
       * than collapse the depth range to nothing. */
      s->z_clip_min = isfinite(lo.f) ? lo.f : 0.0f;
      s->z_clip_max = (hi.u && isfinite(hi.f)) ? hi.f : 16777215.0f;
      if (!(s->z_clip_max > s->z_clip_min)) {
          s->z_clip_min = 0.0f; s->z_clip_max = 16777215.0f;
      }
      s->z_cull = (((M(0x1d78) & 0xF0u) >> 4) == 0u); }
    if (M(0x304)) {
        uint32_t src=M(0x344),dst=M(0x348);
        /* Every distinct blend combination the title actually asks for, logged
         * before the accept test rather than after, because the interesting
         * ones are precisely those this function is about to refuse. Bounded
         * to eight lines: the set is tiny, and a per-draw log would bury it.
         *
         * The reason it exists: xemu issues DST_COLOR/ZERO (0x306/0x0) during
         * the intro cards, a multiply blend of the sort a fade-to-black uses.
         *
         * MEASURED 13 Sep 2026, and the answer was not the one this comment
         * used to record. Our guest asks for exactly that pair too, and it was
         * being refused: 6034 draws deleted in a 150 s run, onset the moment
         * the tutorial scene loads, recurring about once a frame. A refusal
         * here is not a degraded draw, it is no draw at all -- prepare_texture
         * _copy returns an error and nv2a_pb_exec.c drops the batch before
         * either sink sees it -- so each one is a whole flat piece of the
         * scene that never gets painted. That is now implemented rather than
         * refused; what remains refused is listed against blend_factor. */
        {
            static uint32_t seen[8]; static int n; int i;
            uint32_t key = (src<<16) ^ dst ^ (M(0x350)<<1);
            for (i=0;i<n;i++) if (seen[i]==key) break;
            if (i==n && n<8) {
                seen[n++]=key;
                fprintf(stderr, "  [BLEND] enable=%u src=0x%X dst=0x%X eq=0x%X%s\n",
                        M(0x304), src, dst, M(0x350),
                        (M(0x304)!=1 || !nv2a_texture_copy_blend_factor_supported(src)
                         || !nv2a_texture_copy_blend_factor_supported(dst)
                         || M(0x350)!=0x8006) ? "  REFUSED" : "");
            }
        }
        if (M(0x304)!=1 || !nv2a_texture_copy_blend_factor_supported(src)
                || !nv2a_texture_copy_blend_factor_supported(dst)
                || M(0x350)!=0x8006) return "blending";
        s->blend=1;s->blend_src=src;s->blend_dst=dst;
    }
    if (M(0x308)) {
        if (M(0x308)!=1 || (M(0x39c)!=0x404 && M(0x39c)!=0x405 && M(0x39c)!=0x408)
                || (M(0x3a0)!=0x900 && M(0x3a0)!=0x901)) return "face culling";
        s->cull_face=M(0x39c); s->front_cw=M(0x3a0)==0x900;
    }
    if (M(0x30c) && (M(0x30c)!=1 || M(0x354)<0x200 || M(0x354)>0x207 || M(0x35c)>1
                || (M(0x290)&0x11000) || (M(0x208)&0xf0)!=0x20)) return "depth test";
    s->depth_test=M(0x30c); s->depth_write=M(0x35c)!=0; s->depth_func=M(0x354);
    s->depth_handle=M(0x198); s->depth_offset=M(0x214); s->depth_pitch=M(0x20c)>>16;
    if (M(0x32c)) {
        static const uint32_t operations[]={0,0x1e00,0x1e01,0x1e02,0x1e03,0x150a,0x8507,0x8508};
        uint32_t op[3]={M(0x370),M(0x374),M(0x378)};
        if(M(0x32c)!=1 || M(0x364)<0x200 || M(0x364)>0x207) return "stencil test";
        for(unsigned i=0;i<3;++i) {
            unsigned supported=0;
            for(unsigned j=0;j<sizeof(operations)/sizeof(operations[0]);++j) supported|=op[i]==operations[j];
            if(!supported) return "stencil operation";
        }
        s->stencil_test=1;s->stencil_write=(M(0x290)&1)!=0;s->stencil_mask=M(0x360);
        s->stencil_func=M(0x364);s->stencil_ref=M(0x368);s->stencil_func_mask=M(0x36c);
        s->stencil_fail=op[0];s->stencil_zfail=op[1];s->stencil_zpass=op[2];
    }
    /* THE SECOND FOG GATE (G53). The final-combiner gate at the top was
     * opened for fog and this one, further down, still refused every draw
     * with FOG_ENABLE set: 465,058 draws in the player's Rokkaku session
     * after the first fix. The unit tests had built NV2ATextureCopy by hand
     * and never came through here; metal_fog_test now does. */
    if (M(0x2a4) && !fog_model_on()) return "fog";
    if (M(0x324) || M(0x338)) return "polygon smoothing / offset";
    if (M(0x17bc)) return "logic op";
    if (M(0x37c)!=0x1d01 || M(0x38c)!=0x1b02 || M(0x390)!=0x1b02)
        return "shade / polygon mode";
    if (M(0x358)!=0x01010101 || M(0x2b4)) return "colour mask / window clip";
    if (s->untextured) goto target_state;
    { const char *error=nv2a_texture_copy_prepare_image(m,0,s); if(error) return error; }
target_state:
    s->dither=M(0x310)!=0;
    uint32_t format=M(0x208);
    if (((format>>8)&15)!=1 || (format&0x0000f000)) return "nonlinear / multisample target";
    if ((format&15)==3) s->target_bpp=2;
    else if ((format&15)==8) s->target_bpp=4;
    else return "target colour format";
    s->target_handle=M(0x194); s->target_offset=M(0x210);
    s->target_pitch=M(0x20c)&0xffff;
    s->clip_x=M(0x200)&0xffff; s->clip_w=M(0x200)>>16;
    s->clip_y=M(0x204)&0xffff; s->clip_h=M(0x204)>>16;
    if (!s->clip_w || !s->clip_h
            || s->target_pitch<(s->clip_x+s->clip_w)*s->target_bpp)
        return "target dimensions / pitch";
    if ((s->depth_test || s->stencil_test) && s->depth_pitch<(s->clip_x+s->clip_w)*4u)
        return "depth dimensions / pitch";
    /* The window clip is a SCISSOR, not a reason to refuse. Inclusive on
     * both ends (type 0; the exclusive type is refused above with the colour
     * mask). Intersect it with the surface clip and hand the rectangle to the
     * rasteriser; only an empty rectangle has nothing to draw. */
    {
        uint32_t x0=M(0x2c0)&0xfff, x1=(M(0x2c0)>>16)&0xfff;
        uint32_t y0=M(0x2e0)&0xfff, y1=(M(0x2e0)>>16)&0xfff;
        if (x0<s->clip_x) x0=s->clip_x;
        if (y0<s->clip_y) y0=s->clip_y;
        if (x1>s->clip_x+s->clip_w-1) x1=s->clip_x+s->clip_w-1;
        if (y1>s->clip_y+s->clip_h-1) y1=s->clip_y+s->clip_h-1;
        if (x0>x1 || y0>y1) return "empty window clip";
        s->wc_x0=x0; s->wc_y0=y0; s->wc_x1=x1; s->wc_y1=y1;
    }
    return NULL;
}
/* WHICH format is being refused, not merely that one was.
 *
 * The player's 21 Sep run rejected 69,950 draws of 3,118,918 -- 2.2% of
 * everything the title tried to rasterise -- and the log said only
 * "texture format / mip layout". That names the gate, not the input, so it
 * cannot say whether the missing pixels are one unsupported format or
 * twenty. Wall graffiti does not render at all, and a decal is exactly the
 * kind of surface that would arrive in a format this gate does not list.
 *
 * Counters only, printed once with the existing [TEXTURE] report. */

void nv2a_texture_copy_census(void)
{
    unsigned f;
    int approxed = 0;
    {   /* AN APPROXIMATION THAT SUCCEEDS IS STILL NOT THE HARDWARE, so it has
         * to keep this census alive on its own. Placed before the early-out
         * because the first version of RECOMP_TEXMODE_APPROX sat after it:
         * the moment the approximation removed the last refusal, the whole
         * report went silent and took the record of what had been
         * approximated with it. Working and invisible is not better than
         * broken and visible. */
        unsigned m, u;
        for (m = 0; m < 32 && !approxed; ++m)
            for (u = 0; u < 4 && !approxed; ++u) approxed = s_texmode_approx[m][u] != 0;
    }
    /* THE SWITCH STATE OUTLIVES THE SILENCE RULE, because the arm that needs
     * it is the arm with nothing to report. The handover's A/B is
     * "RECOMP_TEXMODE_APPROX=0; if the flicker persists it is not mine", and
     * the =0 arm can legitimately refuse nothing and approximate nothing --
     * at which point every line below is suppressed and the run cannot say
     * which arm it was. ab_score.py then compares two arms that name no
     * switch state and reports them as the same arm. One line is not the
     * table of zeroes the rule is about. */
    fprintf(stderr, "[COMBINER] RECOMP_TEXMODE_APPROX %s\n",
            texmode_approx_on() ? "on" : "OFF");
    fprintf(stderr, "[COMBINER] RECOMP_TEXMODE_BUMP %s\n",
            texmode_bump_on() ? "on" : "OFF");
    {   /* Implemented is not approximated, so these are not in the table
         * below; they are counted so an A/B can see the arm reached them. */
        unsigned m, u;
        for (m = 6; m < 8; ++m)
            for (u = 1; u < 4; ++u)
                if (s_texmode_bump[m][u])
                    fprintf(stderr, "[COMBINER]   unit %u mode %u  x%lu   drawn WITH the"
                            " displacement (RECOMP_TEXMODE_BUMP)\n", u, m, s_texmode_bump[m][u]);
    }

    /* Silent when there is nothing to report: a clean run should not carry a
     * table of zeroes that a later reader has to check is a table of zeroes. */
    if (!s_reject_hdr && !s_reject_dma && !s_reject_combiner_out && !s_rej_final
            && !s_rej_control && !s_rej_outreg && !s_rej_inreg && !s_rej_const
            && !s_rej_texmode && !s_rej_texstage && !s_rej_alpha && !approxed)
        return;
    fprintf(stderr, "[TEXFMT] gate refusals by texture format"
            " (accepted: 0x11/0x12/0x1E linear, 0x0C dxt1, 0x0E dxt3, 0x06/0x07 rgba8,"
            " 0x03 x1r5g5b5, 0x04 a4r4g4b4)\n");
    fprintf(stderr, "[TEXFMT]   header/mip-layout bits wrong=%lu  dma class wrong=%lu"
            "  combiner output=%lu\n",
            s_reject_hdr, s_reject_dma, s_reject_combiner_out);
    for (f = 0; f < 256; ++f)
        if (s_fmt_seen[f] || s_fmt_rejected[f])
            fprintf(stderr, "[TEXFMT]   format 0x%02X  seen=%lu  REFUSED=%lu%s\n",
                    f, s_fmt_seen[f], s_fmt_rejected[f],
                    s_fmt_rejected[f] ? "   <-- never reaches the screen" : "");
    for (f = 0; f < 4; ++f)
        if (s_dma_rejected[f])
            fprintf(stderr, "[TEXFMT]   dma class %u refused=%lu\n", f, s_dma_rejected[f]);

    nv2a_fog_census();
    fprintf(stderr, "[COMBINER] refusals: final-cw=%lu control=%lu output-mode=%lu"
            " output-reg=%lu input-reg=%lu constant=%lu texmode=%lu texstage=%lu"
            " alpha-test=%lu\n",
            s_rej_final, s_rej_control, s_reject_combiner_out, s_rej_outreg,
            s_rej_inreg, s_rej_const, s_rej_texmode, s_rej_texstage, s_rej_alpha);
    for (f = 0; f < 10; ++f)
        if (s_rej_count_hist[f])
            fprintf(stderr, "[COMBINER]   refused with stage count %u: %lu\n",
                    f, s_rej_count_hist[f]);
    {   /* What was DRAWN FLAT rather than dropped. Reported whether or not
         * anything was refused: an approximation that silently succeeds is
         * still a difference from the hardware and has to be visible here.
         *
         * THE SWITCH NAMES ITSELF whether or not it did anything. ab_score.py
         * refuses to compare two arms that report the same switch state, and
         * it can only check a switch that says what it read -- which is the
         * difference between "I set the variable" and "the model read it".
         * The state itself is printed above, before the early-out. */
        unsigned m, u;
        if (approxed) {
            fprintf(stderr, "[COMBINER]   texture shader modes APPROXIMATED as"
                            " plain 2D (RECOMP_TEXMODE_APPROX):\n");
            for (m = 0; m < 32; ++m)
                for (u = 0; u < 4; ++u)
                    if (s_texmode_approx[m][u])
                        fprintf(stderr, "[COMBINER]     unit %u mode %2u  x%lu"
                                "   <-- drawn without the displacement\n",
                                u, m, s_texmode_approx[m][u]);
        }
    }
    if (s_rej_texmode) {
        unsigned m, u;
        fprintf(stderr, "[COMBINER]   texture shader modes on refused draws"
                " (mode 0 = off, 1 = plain 2D, >1 unimplemented):\n");
        for (m = 0; m < 32; ++m)
            for (u = 0; u < 4; ++u)
                if (s_texmode_seen[m][u])
                    fprintf(stderr, "[COMBINER]     unit %u mode %2u  x%lu%s\n",
                            u, m, s_texmode_seen[m][u],
                            m > 1 ? "   <-- this is what is refused" : "");
    }
    for (f = 0; f < s_bad_ocw_used; ++f) {
        uint32_t w = s_bad_ocw[f];
        fprintf(stderr, "[COMBINER]   refused output word 0x%08X  x%lu%s\n",
                w, s_bad_ocw_n[f],
                w==0x000820D0u ? "   <-- the graffiti shader" : "");
        /* Which bits above 0xfff, named where this tree agrees on them.
         * 12/13/14 and 15..17 are decoded identically by d3d8_combiners.c and
         * by combiner_output() here. 18 and up are NOT: d3d8_combiners.c
         * calls them "reserved/unused" and this word has bit 19 set, so they
         * are printed raw rather than guessed at. */
        fprintf(stderr, "[COMBINER]     dst cd=%u ab=%u sum=%u |%s%s%s"
                " map=%u | UNMODELLED bits 18+ = 0x%05X\n",
                w & 15u, (w >> 4) & 15u, (w >> 8) & 15u,
                (w & (1u<<12)) ? " CD_DOT" : "",
                (w & (1u<<13)) ? " AB_DOT" : "",
                (w & (1u<<14)) ? " MUX" : "",
                (w >> 15) & 7u, w >> 18);
    }
    /* Items 2 and 3 of the blocker list, which the early return hides. */
    {
        unsigned r; int any = 0;
        fprintf(stderr, "[COMBINER]   destination registers named by refused"
                " programs:");
        for (r = 0; r < 16; ++r)
            if (s_bad_dst_reg[r]) {
                fprintf(stderr, " %u=%lu%s", r, s_bad_dst_reg[r],
                        (r >= 8 && r <= 11) ? "(TEX!)" : "");
                any = 1;
            }
        fprintf(stderr, "%s\n", any ? "" : " none");
        if (s_bad_c0_used) {
            fprintf(stderr, "[COMBINER]   nonzero per-stage constants:");
            for (r = 0; r < s_bad_c0_used; ++r)
                fprintf(stderr, " 0x%08X", s_bad_c0[r]);
            fprintf(stderr, "   <-- blocker 3 IS live\n");
        } else {
            fprintf(stderr, "[COMBINER]   per-stage constants all zero"
                    " -- blocker 3 is NOT live\n");
        }
    }
    fflush(stderr);
}

const char *nv2a_texture_copy_prepare_image(const uint32_t m[2048], unsigned unit, NV2ATextureCopy *s)
{
    if(unit>3) return "texture unit";
    unsigned b=0x1b00+64*unit;
    uint32_t control=M(b+12), f=M(b+4), dma=f&3, format=(f>>8)&255;
    if ((control&0xc000003fu)!=0x40000000u || (control&0x3ffc0000u))
        return "texture control / colour key / alpha kill";
    s_fmt_seen[format]++;
    if ((f&0xf00000fcu)!=0x28 || (dma!=1 && dma!=2)) {
        if ((f&0xf00000fcu)!=0x28) s_reject_hdr++;
        else { s_reject_dma++; s_dma_rejected[dma & 3]++; }
        s_fmt_rejected[format]++;
        return "texture format / mip layout";
    }
    s->levels=(f>>16)&15;
    if(!s->levels) return "texture mip levels";
    if(s->levels>1 && (control&0x3ffc0u)!=0x3ffc0u)
        return "texture maximum LOD clamp";
    if(format==0x11 || format==0x12 || format==0x1e) {
        /* LU_IMAGE_R5G6B5 (0x11), LU_IMAGE_A8R8G8B8 (0x12) and
         * LU_IMAGE_X8R8G8B8 (0x1E): pitch-linear image rectangles, sized by
         * the image-rectangle register and addressed in TEXELS, not 0..1 --
         * xemu binds all three as rectangle textures. The two 32-bit ones were
         * refused until 24 Sep 2026, and they are Roboy's graffiti studio: the
         * canvas, brush preview and palette are 0x12 textures the game fills
         * by CPU writes, and 894 of 14,911 editor batches were dropped, so
         * nothing painted. 0x1E is 0x12 with the top byte undefined, forced
         * opaque as the swizzled 0x07 is. */
        if(s->levels!=1) return "linear texture mip levels";
        s->lin32=format==0x12 ? 1u : format==0x1e ? 2u : 0u;
        s->width=M(b+28)>>16; s->height=M(b+28)&65535; s->pitch=M(b+16)>>16;
        if(!s->width || !s->height || s->pitch<s->width*(s->lin32 ? 4u : 2u))
            return "texture dimensions / pitch";
    } else if(format==0xc || format==0xe || format==6 || format==7 || format==3
              || format==4) {
        /* 0x07 SZ_X8R8G8B8, 0x03 SZ_X1R5G5B5 and 0x04 SZ_A4R4G4B4 are
         * swizzled like the others, so they take the log2 dimensions here
         * rather than the linear image-rectangle path. Refusing the first two
         * dropped 2.2% of every draw in the player's 21 Sep session. 0x04 is
         * the last member of {0x0C,0x0E,0x03,0x06,0x07,0x04}, the complete set
         * an enumeration of the 1,056 shipped .dat containers says this title
         * can produce -- 11 textures, e.g. Media/Stage/Stg52_t.dat. */
        s->dxt1=format==0xc; s->dxt3=format==0xe;
        s->rgba8=(format==6 || format==7);
        s->xrgb8=(format==7); s->sz16=(format==3 || format==4);
        s->argb4=(format==4);
        unsigned lw=(f>>20)&15,lh=(f>>24)&15;
        if(lw>12 || lh>12 || s->levels>1+(lw>lh?lw:lh)) return "texture dimensions / pitch";
        s->width=1u<<lw; s->height=1u<<lh;
        s->pitch=s->dxt1 ? ((s->width+3)/4)*8 :
            s->dxt3 ? ((s->width+3)/4)*16 :
            s->sz16 ? s->width*2 : s->width*4;
    } else { s_fmt_rejected[format]++; return "texture format / mip layout"; }
    /* Repeat or clamp-to-edge; all three wrap components must agree. */
    if(M(b+8)==0x10101 && format!=0x11 && format!=0x12 && format!=0x1e) s->repeat=1;
    else if(M(b+8)!=0x10303 && M(b+8)!=0x30303) return "texture address mode";
    uint32_t filter=M(b+20), min=(filter>>16)&255, mag=(filter>>24)&15;
    if((filter&0xf000e000)!=0x2000 || min<1 || min>6 || (mag!=1 && mag!=2)) return "texture filter / channel sign";
    s->linear=mag==2; s->min_filter=min;
    s->lod_bias=(float)((int32_t)((filter&8191)<<19)>>19)/256.0f;
    s->texture_handle=M(dma==1?0x184:0x188); s->texture_offset=M(b);
    return NULL;
}
#undef M

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
/* HOW FAR THIS SCAN ACTUALLY WALKS, counted rather than guessed.
 *
 * nv2a_dma_resolve is a linear scan of the RAMHT hash table -- 512 to 4096
 * eight-byte entries depending on NV_PFIFO_RAMHT_SIZE -- and prepare_texture_copy
 * calls it two to five times per draw, so at the 69-180 draws a frame the
 * [STAGE] line reports that is a few hundred scans a frame. Whether that costs
 * nothing or half a millisecond depends on ONE number nobody had: how many
 * entries a scan walks before it stops.
 *
 * It is a wide unknown. A hit stops at the matching entry, so it costs whatever
 * slot the title's handle landed in; a MISS walks the whole table. MEASURED on
 * this host at the build's own -O2, by
 * diagnostics/jsrf_first_fault/dma_resolve_stats_test.c --bench:
 * 0.54 ns per entry scanned, flat across table sizes -- a linear walk is the
 * one access pattern a prefetcher never misses. So the cost of every resolve in
 * a frame is entries_scanned * 0.54 ns, and the span between "handles sit at
 * slot 3" and "every scan misses a 4096-entry table" is 0.001 ms and 0.52 ms a
 * frame. Three orders of magnitude is not a range anyone can optimise against.
 *
 * Counting entries rather than timing them is deliberate. A timer around a
 * call this short would be dominated by the 21 ns clock read (see
 * pb_walk_on), and would measure the instrument. A counter costs one subtract
 * and three adds per CALL -- not per entry -- and is exact.
 *
 * POSITIVE CONTROL. `scans` is the control for `entries`: every scan that runs
 * at all examines at least one entry, so entries==0 with scans>0 is impossible
 * and would mean the counter is wired to the wrong variable, not that the scan
 * is free. `scans` in turn must track the prepare stage's call count in
 * [STAGE], at two to five per draw PLUS one per depth clear -- clear_surface
 * resolves the zeta DMA object the same way, and that scan is already inside
 * the clear timer, so it inflates scans/frame without inflating prepare. A
 * zero here with a moving prepare count means this instrument is dead; both at
 * zero means the draw path is not being reached at all, which is a different
 * bug.
 *
 * Plain non-atomic statics: the executor is single-threaded and these are
 * diagnostics. A torn count would misreport a number nobody acts on directly.
 */
static unsigned long long s_dma_scans, s_dma_entries, s_dma_misses, s_dma_worst;

void nv2a_dma_resolve_stats(unsigned long long *scans, unsigned long long *entries,
                            unsigned long long *misses, unsigned long long *worst)
{
    if (scans) *scans=s_dma_scans;
    if (entries) *entries=s_dma_entries;
    if (misses) *misses=s_dma_misses;
    if (worst) *worst=s_dma_worst;
}

int nv2a_dma_resolve(const uint8_t *ramin, size_t size, uint32_t ramht,
                     uint32_t handle, uint32_t *base, uint32_t *limit)
{
    size_t start=((ramht>>4)&31)<<12, bytes=4096u<<((ramht>>16)&3);
    if (!ramin || start>size || bytes>size-start || !handle) return 0;
    /* Counted before the loop so an argument-rejected call above is NOT
     * counted as a scan: it examined no entries, and folding it in would pull
     * the entries-per-scan average down towards zero and make the table look
     * cheaper to walk than it is. */
    ++s_dma_scans;
    for (size_t off=start; off<start+bytes; off+=8) {
        uint32_t context=read32(ramin+off+4);
        /* The CPU method sink currently runs channel zero only. */
        if (read32(ramin+off)!=handle || !(context&0x80000000)
                || (context&0x1f000000) || (context&0x00020000)) continue;
        {
            size_t walked=(off-start)/8+1;
            s_dma_entries+=walked;
            if (walked>s_dma_worst) s_dma_worst=walked;
        }
        size_t instance=(context&0xffff)<<4;
        if (instance>size || 16>size-instance) return 0;
        uint32_t flags=read32(ramin+instance), frame=read32(ramin+instance+8);
        if ((flags&0xfff)!=0x3d || (flags&0x30000)) return 0;
        *base=((frame&0xfffff000) | (flags>>20)) & 0x07ffffff;
        *limit=read32(ramin+instance+4);
        return 1;
    }
    /* Fell off the end: the whole table was walked. This is the expensive case
     * and the one a memoised lookup could not help with anyway -- there is no
     * entry to cache -- so keeping it separate is what says whether the cost is
     * cacheable at all. */
    s_dma_entries+=bytes/8;
    if (bytes/8>s_dma_worst) s_dma_worst=bytes/8;
    ++s_dma_misses;
    return 0;
}

size_t nv2a_texture_copy_texture_bytes(const NV2ATextureCopy *s)
{
    if (s->untextured) return 0;
    size_t bytes=0;
    unsigned w=s->width,h=s->height;
    for(unsigned l=0;l<(s->levels?s->levels:1);++l) {
        /* sz16 is w*h*2 PER LEVEL: its pitch follows the level's width.
         * s->pitch*h is level 0's pitch on every level, which over-asks by
         * a third on a full chain and refused mipped 0x03/0x04 textures whose
         * true extent fits the bytes given. */
        bytes+=s->dxt1 ? (size_t)((w+3)/4)*((h+3)/4)*8 :
            s->dxt3 ? (size_t)((w+3)/4)*((h+3)/4)*16 :
            s->rgba8 ? (size_t)w*h*4 : s->sz16 ? (size_t)w*h*2 : (size_t)s->pitch*h;
        w=w>1?w/2:1; h=h>1?h/2:1;
    }
    return bytes;
}
static void unpack565(uint32_t v, float rgba[4])
{
    rgba[0]=(float)(v>>11)/31; rgba[1]=(float)((v>>5)&63)/63; rgba[2]=(float)(v&31)/31;
    rgba[3]=1;
}
/* X1R5G5B5: the top bit is undefined, so alpha is opaque rather than read. */
static void unpack555(uint32_t v, float rgba[4])
{
    rgba[0]=(float)((v>>10)&31)/31; rgba[1]=(float)((v>>5)&31)/31;
    rgba[2]=(float)(v&31)/31; rgba[3]=1;
}
/* A4R4G4B4. THE DIVISOR IS 15, NOT 16, and it is the whole of what is easy to
 * get wrong here: a 4-bit channel spans the full range, so 0xF must come back
 * as 1.0 (255), not 0xF0/255 = 0.94. Shifting left by four -- the obvious
 * expansion -- loses 1/17th of the range on every channel and can never reach
 * white. That is a dimming no screenshot review catches. Unlike the other two
 * 16-bit formats this one has REAL alpha. */
static void unpack4444(uint32_t v, float rgba[4])
{
    rgba[0]=(float)((v>>8)&15)/15; rgba[1]=(float)((v>>4)&15)/15;
    rgba[2]=(float)(v&15)/15;      rgba[3]=(float)((v>>12)&15)/15;
}

/* Rectangular Morton order: interleave only the dimensions still active. */
static unsigned swizzle_index(const NV2ATextureCopy *s, unsigned x, unsigned y)
{
    unsigned index=0,bit=0;
    for(unsigned b=1;b<s->width || b<s->height;b<<=1) {
        if(b<s->width)  { if(x&b) index|=1u<<bit; ++bit; }
        if(b<s->height) { if(y&b) index|=1u<<bit; ++bit; }
    }
    return index;
}

static void texel(const NV2ATextureCopy *s, const uint8_t *data, int x, int y, float rgba[4])
{
    if (s->repeat) {
        x=(x%(int)s->width+(int)s->width)%(int)s->width;
        y=(y%(int)s->height+(int)s->height)%(int)s->height;
    } else {
        if (x<0) x=0; else if ((uint32_t)x>=s->width) x=(int)s->width-1;
        if (y<0) y=0; else if ((uint32_t)y>=s->height) y=(int)s->height-1;
    }
    if(s->rgba8) {
        const uint8_t *p=data+4*(size_t)swizzle_index(s,(unsigned)x,(unsigned)y);
        rgba[0]=p[2]/255.0f; rgba[1]=p[1]/255.0f; rgba[2]=p[0]/255.0f;
        /* X8R8G8B8 leaves the top byte undefined; sampling it gives a texture
         * that is transparent wherever the padding happens to be zero. */
        rgba[3]=s->xrgb8 ? 1.0f : p[3]/255.0f;
        return;
    }
    if(s->sz16) {
        const uint8_t *p=data+2*(size_t)swizzle_index(s,(unsigned)x,(unsigned)y);
        uint32_t v=p[0] | (uint32_t)p[1]<<8;
        if(s->argb4) unpack4444(v,rgba); else unpack555(v,rgba);
        return;
    }
    if (s->dxt1 || s->dxt3) {
        /* BC1/BC2 consist of row-major 4x4 blocks, not Morton-swizzled texels. */
        const uint8_t *p=data+(size_t)(y/4)*s->pitch+(x/4)*(s->dxt3?16:8);
        float alpha=1;
        if(s->dxt3) {
            unsigned i=(unsigned)(y&3)*4+(unsigned)(x&3);
            alpha=(float)((p[i/2]>>(4*(i&1)))&15)/15;
            p+=8;
        }
        uint32_t c0=p[0]|(uint32_t)p[1]<<8,c1=p[2]|(uint32_t)p[3]<<8;
        unsigned index=(read32(p+4)>>(2*((y&3)*4+(x&3))))&3;
        if (index<2) { unpack565(index ? c1 : c0,rgba); rgba[3]=alpha; return; }
        if (!s->dxt3 && c0<=c1 && index==3) { memset(rgba,0,4*sizeof(float)); return; }
        float a[4],b[4]; unpack565(c0,a); unpack565(c1,b);
        float weight=!s->dxt3 && c0<=c1 ? .5f :
            (index==2 ? 2.0f/3.0f : 1.0f/3.0f);
        for (int k=0;k<3;++k) rgba[k]=a[k]*weight+b[k]*(1-weight);
        rgba[3]=alpha; return;
    }
    if(s->lin32) {
        /* Linear A8R8G8B8 / X8R8G8B8: BGRA bytes at y*pitch + x*4, which is
         * exactly how the graffiti editor writes its canvas. */
        const uint8_t *p=data+(size_t)y*s->pitch+(size_t)x*4;
        rgba[0]=p[2]/255.0f; rgba[1]=p[1]/255.0f; rgba[2]=p[0]/255.0f;
        rgba[3]=s->lin32==2 ? 1.0f : p[3]/255.0f;
        return;
    }
    const uint8_t *p=data+(size_t)y*s->pitch+x*2;
    uint32_t v=p[0] | (uint32_t)p[1]<<8;
    unpack565(v,rgba);
}
static void sample(const NV2ATextureCopy *s, const uint8_t *data, float u, float v, float rgba[4])
{
    if (s->dxt1 || s->dxt3 || s->rgba8 || s->sz16) {
        /* Nonlinear textures use normalized coordinates, unlike image rectangles. */
        if (s->repeat) { u-=floorf(u); v-=floorf(v); }
        else { u=fmaxf(0,fminf(1,u)); v=fmaxf(0,fminf(1,v)); }
        u*=s->width; v*=s->height;
    }
    /* Clamp before converting to integers, including extreme valid coordinates. */
    u=fmaxf(0, fminf((float)s->width,u));
    v=fmaxf(0, fminf((float)s->height,v));
    if (!s->linear) { texel(s,data,(int)floorf(u),(int)floorf(v),rgba); return; }
    float x=u-0.5f,y=v-0.5f, fx=floorf(x),fy=floorf(y), tx=x-fx,ty=y-fy;
    float p[4][4];
    texel(s,data,(int)fx,(int)fy,p[0]); texel(s,data,(int)fx+1,(int)fy,p[1]);
    texel(s,data,(int)fx,(int)fy+1,p[2]); texel(s,data,(int)fx+1,(int)fy+1,p[3]);
    for (int k=0;k<4;++k) rgba[k]=(p[0][k]*(1-tx)+p[1][k]*tx)*(1-ty)
                                      +(p[2][k]*(1-tx)+p[3][k]*tx)*ty;
}
static void sample_lod(const NV2ATextureCopy *s,const uint8_t *data,float u,float v,float lod,float out[4])
{
    float l=fmaxf(0,lod+s->lod_bias);
    if(s->min_filter<3 || s->levels<2) l=0;
    l=fminf(l,(float)(s->levels?s->levels-1:0));
    unsigned lo=(unsigned)(s->min_filter>=5?floorf(l):floorf(l+.5f));
    unsigned hi=s->min_filter>=5 && lo+1<s->levels?lo+1:lo;
    NV2ATextureCopy t=*s;
    if(lod+s->lod_bias>0 && s->min_filter) t.linear=(s->min_filter%2)==0;
    const uint8_t *p=data;
    float a[4],b[4];
    for(unsigned level=0;level<=hi;++level) {
        if(level==lo) sample(&t,p,u,v,a);
        if(level==hi) {
            /* A single selected mip was already sampled above. Keep the
             * interpolation arithmetic unchanged, but avoid decoding and
             * filtering the same texels twice for every fragment. */
            if(hi==lo) memcpy(b,a,sizeof(b));
            else sample(&t,p,u,v,b);
            break;
        }
        /* The swizzled 16-bit formats are TWO bytes a texel. Walking them at
         * the RGBA8 stride put level 1 at twice its offset -- reading level 2
         * and beyond, or past the chain -- while level 0 looked right. */
        p+=t.dxt1?(size_t)((t.width+3)/4)*((t.height+3)/4)*8:
            t.dxt3?(size_t)((t.width+3)/4)*((t.height+3)/4)*16:
            (size_t)t.width*t.height*(t.sz16?2:4);
        t.width=t.width>1?t.width/2:1; t.height=t.height>1?t.height/2:1;
        t.pitch=t.dxt1?((t.width+3)/4)*8:t.dxt3?((t.width+3)/4)*16:t.width*(t.sz16?2:4);
    }
    float f=hi==lo?0:l-lo;
    for(unsigned k=0;k<4;++k) out[k]=a[k]*(1-f)+b[k]*f;
}

/* BUMPENVMAP (6) and BUMPENVMAP_LUMINANCE (7), xemu pgraph/glsl/psh.c op for
 * op; the MSL bump_sample() in nv2a_metal.m is its twin and metal_bump_test
 * holds the two together. sign3 is xemu's: the channel read back as the
 * two's-complement byte it was stored as, over 127 -- applied to the FILTERED
 * value, which is xemu's own FIXME and what hardware may not do. The channel
 * sign bits in the filter word never reach here: prepare_image refuses any
 * texture that sets them. The LOD is the unperturbed coordinate's. */
static float bump_sign3(float x)
{
    x*=255.0f;
    return x>=128.0f ? (x-256.0f)/127.0f : x/127.0f;
}
static void bump_sample(const NV2ATextureCopy *s, unsigned unit, const NV2ATextureCopy *t,
                        const uint8_t *data, float u, float v, float lod, float regs[16][4])
{
    const float *in=regs[8+s->bump_input[unit]], *mm=s->bump_mat[unit];
    float du=bump_sign3(in[2]), dv=bump_sign3(in[1]);
    float pu=mm[0]*du+mm[2]*dv, pv=mm[1]*du+mm[3]*dv;
    float *out=regs[8+unit];
    sample_lod(t,data,u+pu,v+pv,lod,out);
    if(s->bump[unit]==7) {
        float k=s->bump_scale[unit]*in[0]+s->bump_offset[unit];
        for(unsigned c=0;c<4;++c) out[c]*=k;
    }
}

/* Unpack one mip level into tightly packed RGBA8, in R,G,B,A byte order.
 *
 * A hardware sampler cannot read the guest's Morton-swizzled ARGB8, its
 * block-compressed levels, or its pitch-padded RGB565 image rectangles, so the
 * D3D11 path unpacks each texture once and uploads the result. It goes through
 * the same texel() the CPU rasteriser uses, so the two agree by construction
 * rather than by a second reading of the format documentation.
 *
 * Returns zero without writing anything if the level does not exist, or if
 * either the source or the destination would be overrun. */
int nv2a_texture_copy_decode_level(const NV2ATextureCopy *s, const uint8_t *data,
    size_t size, unsigned level, uint8_t *out, size_t out_size,
    unsigned *out_w, unsigned *out_h)
{
    if(!s || !data || !out || s->untextured) return 0;
    unsigned levels=s->levels?s->levels:1;
    if(level>=levels || !s->width || !s->height) return 0;
    NV2ATextureCopy t=*s;
    /* Decoding addresses exact texels, so the wrap mode cannot matter; force
     * clamp so a rounding error reads an edge texel rather than wrapping. */
    t.repeat=0;
    size_t offset=0;
    for(unsigned l=0;l<level;++l) {
        offset+=t.dxt1?(size_t)((t.width+3)/4)*((t.height+3)/4)*8:
            t.dxt3?(size_t)((t.width+3)/4)*((t.height+3)/4)*16:
            t.rgba8?(size_t)t.width*t.height*4:
            t.sz16?(size_t)t.width*t.height*2:(size_t)t.pitch*t.height;
        t.width=t.width>1?t.width/2:1; t.height=t.height>1?t.height/2:1;
        /* A swizzled level's pitch follows its width; a linear image
         * rectangle's does not, which is why this is not one expression. */
        t.pitch=t.dxt1?((t.width+3)/4)*8:t.dxt3?((t.width+3)/4)*16:
            t.rgba8?t.width*4:t.sz16?t.width*2:t.pitch;
    }
    size_t level_bytes=t.dxt1?(size_t)((t.width+3)/4)*((t.height+3)/4)*8:
        t.dxt3?(size_t)((t.width+3)/4)*((t.height+3)/4)*16:
        t.rgba8?(size_t)t.width*t.height*4:
        t.sz16?(size_t)t.width*t.height*2:(size_t)t.pitch*t.height;
    if(offset>size || level_bytes>size-offset) return 0;
    if((size_t)t.width*t.height*4>out_size) return 0;
    const uint8_t *p=data+offset;
    for(unsigned y=0;y<t.height;++y) for(unsigned x=0;x<t.width;++x) {
        float rgba[4];
        texel(&t,p,(int)x,(int)y,rgba);
        uint8_t *q=out+((size_t)y*t.width+x)*4;
        for(unsigned k=0;k<4;++k) q[k]=(uint8_t)(fminf(1,fmaxf(0,rgba[k]))*255+.5f);
    }
    if(out_w) *out_w=t.width;
    if(out_h) *out_h=t.height;
    return 1;
}

static float combiner_input(unsigned input,unsigned channel,const float regs[16][4])
{
    unsigned source=input&15;
    float x=regs[source][input&16?3:channel];
    switch(input>>5) {
    case 0:return fmaxf(0,x);
    case 1:return 1-fminf(1,fmaxf(0,x));
    case 2:return 2*fmaxf(0,x)-1;
    case 3:return 1-2*fmaxf(0,x);
    case 4:return fmaxf(0,x)-.5f;
    case 5:return .5f-fmaxf(0,x);
    case 6:return x;
    default:return -x;
    }
}

/* One combiner stage's outputs, all four channels at once.
 *
 * IT HAS TO BE ALL FOUR AT ONCE. A dot product collapses RGB to one scalar,
 * and blue-to-alpha copies the BLUE result into a register's alpha -- neither
 * can be expressed by a function that sees one channel at a time, which is
 * what this used to be. `ab` and `cd` arrive as [r,g,b,a].
 *
 * Mapping is applied to the stage's own outputs, after the dot product and
 * before the register write, which is the order the hardware documents. */
static float combiner_map(unsigned mode,float x)
{
    switch(mode) {
    case 1: return x-0.5f;              /* BIAS                */
    case 2: return x*2.0f;              /* SHIFTLEFTBY1        */
    case 3: return (x-0.5f)*2.0f;       /* SHIFTLEFTBY1_BIAS   */
    case 4: return x*4.0f;              /* SHIFTLEFTBY2        */
    case 6: return x*0.5f;              /* SHIFTRIGHTBY1       */
    default: return x;                  /* 0,5,7: identity     */
    }
}

static void combiner_stage_output(float regs[16][4],uint32_t cw,uint32_t aw,
                                  float ab[4],float cd[4],float r0_alpha)
{
    /* THROUGH THE SHARED DECODER. These shifts were right and d3d8_combiners.c's
     * copy of them was inverted, for at least two handovers, because there were
     * two copies. There is one now; this path's numbers are unchanged. */
    NV2AOutputWord C, A;
    nv2a_parse_output_word(cw, &C);
    nv2a_parse_alpha_output_word(aw, &A);
    unsigned cd_dst=C.cd_dst, ab_dst=C.ab_dst, sum_dst=C.sum_dst;
    unsigned a_cd_dst=A.cd_dst, a_ab_dst=A.ab_dst, a_sum_dst=A.sum_dst;
    unsigned cd_dot=C.cd_dot, ab_dot=C.ab_dot;
    unsigned mux=C.mux_sum, map=C.output_map;
    unsigned cd_b2a=C.cd_blue_to_alpha, ab_b2a=C.ab_blue_to_alpha;
    unsigned a_mux=A.mux_sum, a_map=A.output_map;
    float ab_rgb[3], cd_rgb[3], sum_rgb[3];
    unsigned k;

    /* A dot product is RGB-only and broadcasts to all three components. The
     * alpha combiner has no dot; its bits 12/13 are not this. */
    if(ab_dot) {
        float d=ab[0]+ab[1]+ab[2];
        ab_rgb[0]=ab_rgb[1]=ab_rgb[2]=d;
    } else { for(k=0;k<3;++k) ab_rgb[k]=ab[k]; }
    if(cd_dot) {
        float d=cd[0]+cd[1]+cd[2];
        cd_rgb[0]=cd_rgb[1]=cd_rgb[2]=d;
    } else { for(k=0;k<3;++k) cd_rgb[k]=cd[k]; }

    /* MUX selects on R0's alpha rather than summing. */
    for(k=0;k<3;++k)
        sum_rgb[k]= mux ? (r0_alpha>=0.5f?ab_rgb[k]:cd_rgb[k]) : ab_rgb[k]+cd_rgb[k];

    for(k=0;k<3;++k) {
        ab_rgb[k]=combiner_map(map,ab_rgb[k]);
        cd_rgb[k]=combiner_map(map,cd_rgb[k]);
        sum_rgb[k]=combiner_map(map,sum_rgb[k]);
    }

#define WR(dst,ch,v) do { if(dst) regs[dst][ch]=fmaxf(-1,fminf(1,(v))); } while(0)
    for(k=0;k<3;++k) {
        WR(ab_dst,k,ab_rgb[k]);
        WR(cd_dst,k,cd_rgb[k]);
        WR(sum_dst,k,sum_rgb[k]);
    }
    /* The alpha combiner writes only alpha, and has no dot or blue-to-alpha. */
    {
        float a_ab=combiner_map(a_map,ab[3]), a_cd=combiner_map(a_map,cd[3]);
        float a_sum=combiner_map(a_map, a_mux ? (r0_alpha>=0.5f?ab[3]:cd[3])
                                              : ab[3]+cd[3]);
        WR(a_ab_dst,3,a_ab);
        WR(a_cd_dst,3,a_cd);
        WR(a_sum_dst,3,a_sum);
    }
    /* Blue-to-alpha, LAST, because it REPLACES that register's alpha rather
     * than competing with the alpha combiner for it. Ordering it before the
     * alpha block -- which is how this was first written -- lets the alpha
     * combiner overwrite the very value the flag exists to deliver, and the
     * tag would carry the wrong mask while still drawing something plausible.
     *
     * This is what lets a shader move a mask it extracted with a dot product
     * into alpha, which is what the tag decal does. */
    if(ab_b2a) WR(ab_dst,3,ab_rgb[2]);
    if(cd_b2a) WR(cd_dst,3,cd_rgb[2]);
#undef WR
}

static void combine(const NV2ATextureCopy *s,float regs[16][4],float fogd,float out[4])
{
    /* R0 alpha starts with texture zero's alpha on NV2A. */
    regs[12][3]=(s->texture_mask&1)?regs[8][3]:1;
    for(unsigned stage=0;stage<s->combiner_count;++stage) {
        float ab[4],cd[4];
        /* Per-stage constants land in registers 1 and 2 before the stage runs.
         * D3DCOLOR is A8R8G8B8, so the channel order here is not incidental. */
        { uint32_t c0=s->const0[stage], c1=s->const1[stage];
          regs[1][0]=(float)((c0>>16)&255)/255.0f;
          regs[1][1]=(float)((c0>>8)&255)/255.0f;
          regs[1][2]=(float)(c0&255)/255.0f;
          regs[1][3]=(float)((c0>>24)&255)/255.0f;
          regs[2][0]=(float)((c1>>16)&255)/255.0f;
          regs[2][1]=(float)((c1>>8)&255)/255.0f;
          regs[2][2]=(float)(c1&255)/255.0f;
          regs[2][3]=(float)((c1>>24)&255)/255.0f; }
        for(unsigned k=0;k<4;++k) {
            unsigned word=k==3?s->alpha_icw[stage]:s->color_icw[stage];
            unsigned channel=k==3?2:k; /* Alpha ICW selects blue or alpha. */
            float a=combiner_input(word>>24,channel,regs), b=combiner_input((word>>16)&255,channel,regs);
            float c=combiner_input((word>>8)&255,channel,regs),d=combiner_input(word&255,channel,regs);
            ab[k]=a*b;cd[k]=c*d;
        }
        combiner_stage_output(regs,s->color_ocw[stage],s->alpha_ocw[stage],
                              ab,cd,regs[12][3]);
    }
    if(s->final_general) {
        float f = s->fog_enable ? nv2a_fog_factor(s->fog_mode, s->fog_p0, s->fog_p1, fogd) : 1.0f;
        if (!(f >= 0)) f = 0;
        nv2a_final_combine(s, regs, f, out);
        return;
    }
    for(unsigned k=0;k<4;++k)
        out[k]=fmaxf(0,fminf(1,regs[12][k]+(s->add_specular && k<3?regs[5][k]:0)));
}
static unsigned quantize(float value, unsigned max)
{
    return (unsigned)(fminf(1,fmaxf(0,value))*max+0.5f);
}
static int compare_value(uint32_t func,uint32_t source,uint32_t target)
{
    if(!func) func=0x203; /* Directly constructed test state defaults to LEQUAL. */
    switch(func) {
    case 0x200:return 0;case 0x201:return source<target;case 0x202:return source==target;
    case 0x203:return source<=target;case 0x204:return source>target;case 0x205:return source!=target;
    case 0x206:return source>=target;default:return 1;
    }
}
static uint8_t stencil_result(uint32_t op,uint8_t old,uint8_t ref)
{
    switch(op) {
    case 0:return 0;case 0x1e01:return ref;case 0x1e02:return old==255?255:(uint8_t)(old+1);
    case 0x1e03:return old?old-1:0;case 0x150a:return (uint8_t)~old;
    case 0x8507:return (uint8_t)(old+1);case 0x8508:return (uint8_t)(old-1);default:return old;
    }
}
static void stencil_update(const NV2ATextureCopy *s,uint8_t *zeta,uint32_t op)
{
    if(!s->stencil_write) return;
    uint8_t mask=(uint8_t)s->stencil_mask,old=zeta[0];
    uint8_t next=stencil_result(op,old,(uint8_t)s->stencil_ref);
    zeta[0]=(uint8_t)((old&~mask)|(next&mask));
}
/* A blend factor is per-channel, not a scalar. DST_COLOR and SRC_COLOR
 * cannot be expressed as one number, and writing them as one is what forced
 * the accept test below to refuse a multiply blend outright -- which deleted
 * the whole draw. `src` and `dst` are the two colours; the result is the
 * four-channel weight this factor contributes.
 *
 * The destination ALPHA factors (0x304, 0x305) and SRC_ALPHA_SATURATE (0x308)
 * are deliberately absent, and the accept test still refuses them: the Metal
 * sink keeps the 24-bit depth value in the surface's alpha channel, so there
 * is no destination alpha there to read. Adding them means giving that path a
 * real alpha channel first. */
static void blend_factor(uint32_t factor,const float src[4],const float dst[4],float out[4])
{
    int k;
    switch(factor) {
    case 0x000: for(k=0;k<4;++k) out[k]=0;         break;  /* ZERO */
    case 0x001: for(k=0;k<4;++k) out[k]=1;         break;  /* ONE */
    case 0x300: for(k=0;k<4;++k) out[k]=src[k];    break;  /* SRC_COLOR */
    case 0x301: for(k=0;k<4;++k) out[k]=1-src[k];  break;  /* ONE_MINUS_SRC_COLOR */
    case 0x302: for(k=0;k<4;++k) out[k]=src[3];    break;  /* SRC_ALPHA */
    case 0x303: for(k=0;k<4;++k) out[k]=1-src[3];  break;  /* ONE_MINUS_SRC_ALPHA */
    case 0x306: for(k=0;k<4;++k) out[k]=dst[k];    break;  /* DST_COLOR */
    case 0x307: for(k=0;k<4;++k) out[k]=1-dst[k];  break;  /* ONE_MINUS_DST_COLOR */
    /* Unreachable: the accept test gates the set. Contribute nothing rather
     * than silently standing in for ONE_MINUS_SRC_ALPHA, which is what the
     * old default did. */
    default:    for(k=0;k<4;++k) out[k]=0;         break;
    }
}
/* True when the screen-space winding of a triangle is the reverse of its true
 * orientation, because an odd number of its vertices sit behind the camera.
 *
 * area() runs on positions the GUEST already perspective-divided. The true
 * orientation is the sign of the 3x3 determinant of the homogeneous
 * coordinates, and writing x_clip = ndc_x * w that determinant factors exactly
 * into (screen area) * w0*w1*w2 -- so the correct test is the screen area
 * times sign(w0*w1*w2). Measured over random clip-space triangles: with
 * exactly one negative w the uncorrected test is wrong EVERY time, not
 * sometimes.
 *
 * Counted as a parity of sign bits rather than an actual product, because
 * w reaches 1e31 here and the product overflows to infinity. w == 0 counts as
 * positive, which leaves the existing behaviour for a vertex exactly on the
 * camera plane.
 *
 * This does NOT resurrect geometry behind the camera. A vertex with w < 0
 * cannot satisfy -w <= x <= w (summing the two gives 2w >= 0), so the GPU's
 * homogeneous clipper removes it: measured, 0 of 20000 random all-negative
 * triangles survive. Correcting the sign only rescues triangles that STRADDLE
 * the camera plane -- the large near ground quads whose loss reads as a black
 * floor -- and those are clipped to their visible part before rasterising. */
int nv2a_texture_copy_winding_flipped(float wa, float wb, float wc)
{
    return (((wa < 0) + (wb < 0) + (wc < 0)) & 1) != 0;
}
/* The set blend_factor implements, and therefore the set prepare_texture_copy
 * may accept. One place, so the three sinks cannot drift apart. */
int nv2a_texture_copy_blend_factor_supported(uint32_t f)
{
    return f==0x000||f==0x001||f==0x300||f==0x301
        || f==0x302||f==0x303||f==0x306||f==0x307;
}
static float edge(const float a[4], const float b[4], float x, float y)
{
    return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);
}
static int inclusive_edge(const float a[4], const float b[4], float sign)
{
    float dy=(b[1]-a[1])*sign, dx=(b[0]-a[0])*sign;
    return dy<0 || (dy==0 && dx>0);
}
int nv2a_texture_copy_triangle_depth(const NV2ATextureCopy *s,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    uint8_t *depth, size_t depth_size,
    const float a[16][4], const float b[16][4], const float c[16][4])
{
    if (!target
            || (!s->untextured && (!texture || !s->width || !s->height
                || s->width>65535 || s->height>65535
                || s->pitch<(s->dxt1 ? ((s->width+3)/4)*8 :
                    s->dxt3 ? ((s->width+3)/4)*16 : s->width*2u)))
            || (s->target_bpp!=2 && s->target_bpp!=4)
            || s->target_pitch<(uint64_t)(s->clip_x+s->clip_w)*s->target_bpp
            || nv2a_texture_copy_texture_bytes(s)>texture_size
            || (uint64_t)s->target_pitch*(s->clip_y+s->clip_h)>target_size) return 0;
    if ((s->depth_test || s->stencil_test) && (!depth || s->depth_pitch<(uint64_t)(s->clip_x+s->clip_w)*4
                || (uint64_t)s->depth_pitch*(s->clip_y+s->clip_h)>depth_size)) return 0;
    for(unsigned unit=1;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
        if(!s->extra_stages || !s->extra_texture[unit-1] ||
                nv2a_texture_copy_texture_bytes(&s->extra_stages[unit-1])>s->extra_size[unit-1]) return 0;
    }
    const float (*v[3])[4]={a,b,c};
    for (int i=0;i<3;++i) {
        for (int k=0;k<4;++k)
            if (!isfinite(v[i][0][k])
                    || (!s->untextured && !isfinite(v[i][NV2A_VSH_OUT_T0][k])))
                return 0;
        if (!(v[i][0][3]>0) || !isfinite(v[i][NV2A_VSH_OUT_D0][3])
                || (!s->untextured && !(v[i][NV2A_VSH_OUT_T0][3]>0))) return 0;
        if (s->modulate)
            for (int k=0;k<3;++k)
                if (!isfinite(v[i][NV2A_VSH_OUT_D0][k])) return 0;
    }
    if(s->combiner_count) for(unsigned i=0;i<3;++i) {
        for(unsigned k=0;k<4;++k)
            if(!isfinite(v[i][NV2A_VSH_OUT_D0][k]) || !isfinite(v[i][NV2A_VSH_OUT_D1][k])) return 0;
        for(unsigned unit=0;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
            for(unsigned k=0;k<4;++k) if(!isfinite(v[i][NV2A_VSH_OUT_T0+unit][k])) return 0;
            if(!(v[i][NV2A_VSH_OUT_T0+unit][3]>0)) return 0;
        }
    }
    float area=edge(a[0],b[0],c[0][0],c[0][1]);
    if (!isfinite(area)) return 0;
    if (area==0) return 1;
    /* framebuffer Y increases downwards; the w parity corrects a winding
     * the guest's own perspective divide reversed. NOTE: this path also
     * rejects any vertex with w <= 0 outright before reaching here, so a
     * straddling triangle is still lost on the software fallback -- that
     * needs real near-plane clipping, which the GPU sinks get for free. */
    int front = nv2a_texture_copy_front_facing(s, area, v[0][0][3],
                                               v[1][0][3], v[2][0][3]);
    if (nv2a_texture_copy_culled(s, front)) return 1;
    /* The window clip, already intersected with the surface clip. */
    uint32_t wx0,wy0,wx1,wy1; nv2a_texture_copy_window(s,&wx0,&wy0,&wx1,&wy1);
    float left=(float)wx0,right=(float)(wx1+1);
    float top=(float)wy0,bottom=(float)(wy1+1);
    int x0=(int)floorf(fmaxf(left,fminf(right,fminf(a[0][0],fminf(b[0][0],c[0][0])))));
    int x1=(int)ceilf(fmaxf(left,fminf(right,fmaxf(a[0][0],fmaxf(b[0][0],c[0][0])))));
    int y0=(int)floorf(fmaxf(top,fminf(bottom,fminf(a[0][1],fminf(b[0][1],c[0][1])))));
    int y1=(int)ceilf(fmaxf(top,fminf(bottom,fmaxf(a[0][1],fmaxf(b[0][1],c[0][1])))));
    float sign=area>0 ? 1.0f : -1.0f;
    int include[3]={inclusive_edge(b[0],c[0],sign),inclusive_edge(c[0],a[0],sign),inclusive_edge(a[0],b[0],sign)};
    /* A common post-process is an oversized triangle copying texels 1:1.
     * Prove that all pixel centres in its bounds are covered before using
     * row copies. This also preserves RGB565 quantisation exactly. */
    /* !untextured, because the copy below reads `texture`, and an untextured
     * batch is entitled to pass NULL for it -- every other guard above lets it
     * through, and the bounds test that incidentally covered this only did so
     * because an untextured state also leaves width and height zero.
     * !sz16 likewise: the row copy assumes LINEAR 565 bytes, and 0x03/0x04
     * are swizzled and differently packed. !lin32 too: 0x12/0x1E are
     * linear, so they pass every other test here, and the graffiti canvas is
     * exactly the full-screen 1:1 quad this path is for -- it would have
     * memcpy'd BGRA bytes into a 565 target. */
    int direct=!s->combiner_count && !s->untextured && !s->rgba8 && !s->sz16 && !s->lin32 && !s->modulate && !s->dxt1 && !s->dxt3 && !s->alpha_test && !s->blend && !s->depth_test && !s->stencil_test
        && s->target_bpp==2 && x0>=0 && y0>=0 && (uint32_t)x1<=s->width && (uint32_t)y1<=s->height;
    for (int i=0;i<3;++i)
        if (v[i][0][3]!=1 || v[i][NV2A_VSH_OUT_T0][3]!=1
                || v[i][0][0]!=v[i][NV2A_VSH_OUT_T0][0]
                || v[i][0][1]!=v[i][NV2A_VSH_OUT_T0][1]) direct=0;
    for (int corner=0;direct && corner<4;++corner) {
        float px=(corner&1) ? x1-.5f : x0+.5f, py=(corner&2) ? y1-.5f : y0+.5f;
        float e[3]={edge(b[0],c[0],px,py),edge(c[0],a[0],px,py),edge(a[0],b[0],px,py)};
        for (int i=0;i<3;++i) if (e[i]*sign<0 || (e[i]==0 && !include[i])) direct=0;
    }
    if (direct && x1>x0 && y1>y0) {
        for (int y=y0;y<y1;++y)
            memcpy(target+(size_t)y*s->target_pitch+x0*2, texture+(size_t)y*s->pitch+x0*2, (size_t)(x1-x0)*2);
        return 1;
    }
    for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
        float e[3]={edge(b[0],c[0],x+.5f,y+.5f),edge(c[0],a[0],x+.5f,y+.5f),edge(a[0],b[0],x+.5f,y+.5f)};
        int inside=1;
        for (int i=0;i<3;++i) if (e[i]*sign<0 || (e[i]==0 && !include[i])) inside=0;
        if (!inside) continue;
        float u=0,t=0,q=0,alpha=0,recip=0,diffuse[3]={0};
        double z=0;
        /* Screen-space XYZ retain clip W in oPos.w. Perspective-correct
         * varyings use reciprocal W; PROJECT2D then divides S/T by Q.
         * The observed framebuffer copy has W=Q=1. */
        for (int i=0;i<3;++i) {
            float w=e[i]/area/v[i][0][3];
            z+=(double)e[i]/area*v[i][0][2]; /* post-viewport depth is affine */
            if (!s->untextured) {
                u+=w*v[i][NV2A_VSH_OUT_T0][0]; t+=w*v[i][NV2A_VSH_OUT_T0][1];
                q+=w*v[i][NV2A_VSH_OUT_T0][3];
            }
            alpha+=w*v[i][NV2A_VSH_OUT_D0][3]; recip+=w;
            if (s->modulate)
                for (int k=0;k<3;++k) diffuse[k]+=w*v[i][NV2A_VSH_OUT_D0][k];
        }
        if (!(recip>0)) return 0;
        float rgb[4];
        if(s->combiner_count) {
            float regs[16][4]={{0}}, fogd=0;
            for(unsigned i=0;i<3;++i) {
                float w=e[i]/area/v[i][0][3]/recip;
                for(unsigned k=0;k<4;++k) {
                    regs[4][k]+=w*v[i][NV2A_VSH_OUT_D0][k];
                    regs[5][k]+=w*v[i][NV2A_VSH_OUT_D1][k];
                }
                fogd+=w*v[i][NV2A_VSH_OUT_FOG][0];
            }
            for(unsigned unit=0;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
                const NV2ATextureCopy *t=unit?&s->extra_stages[unit-1]:s;
                const uint8_t *data=unit?s->extra_texture[unit-1]:texture;
                float uv[3][2];
                for(unsigned at=0;at<3;++at) {
                    float sx=0,sy=0,sq=0,sw=0;
                    float px=x+.5f+(at==1),py=y+.5f+(at==2);
                    float weights[3]={edge(b[0],c[0],px,py),edge(c[0],a[0],px,py),edge(a[0],b[0],px,py)};
                    for(unsigned i=0;i<3;++i) {
                        float w=weights[i]/area/v[i][0][3];
                        sx+=w*v[i][NV2A_VSH_OUT_T0+unit][0];
                        sy+=w*v[i][NV2A_VSH_OUT_T0+unit][1];
                        sq+=w*v[i][NV2A_VSH_OUT_T0+unit][3];
                        sw+=w;
                    }
                    if(!isfinite(sq) || sq==0) return 0;
                    /* BUMPENVMAP takes coord.xy as interpolated, with no
                     * projective divide (xemu: pT.xy, not textureProj). */
                    if(s->bump[unit]) sq=sw;
                    if(!isfinite(sq) || sq==0) return 0;
                    uv[at][0]=sx/sq;uv[at][1]=sy/sq;
                    if(!isfinite(uv[at][0]) || !isfinite(uv[at][1])) return 0;
                }
                float dx=hypotf((uv[1][0]-uv[0][0])*t->width,(uv[1][1]-uv[0][1])*t->height);
                float dy=hypotf((uv[2][0]-uv[0][0])*t->width,(uv[2][1]-uv[0][1])*t->height);
                float lod=log2f(fmaxf(0.000001f,fmaxf(dx,dy)));
                if(s->bump[unit]) bump_sample(s,unit,t,data,uv[0][0],uv[0][1],lod,regs);
                else sample_lod(t,data,uv[0][0],uv[0][1],lod,regs[8+unit]);
            }
            combine(s,regs,fogd,rgb);
        } else {
            if (s->untextured) { rgb[0]=rgb[1]=rgb[2]=rgb[3]=1; }
            else {
                if (!(q>0) || !isfinite(u/q) || !isfinite(t/q)) return 0;
                sample(s,texture,u/q,t/q,rgb);
            }
            rgb[3]=fminf(1,fmaxf(0,alpha/recip))*(s->modulate ? rgb[3] : 1);
            if (s->modulate)
                for (int k=0;k<3;++k) rgb[k]=fminf(1,rgb[k]*fmaxf(0,diffuse[k]/recip));
        }
        if (s->alpha_test && quantize(rgb[3],255)<=s->alpha_ref) continue;
        uint8_t *zp=(s->depth_test||s->stencil_test) ? depth+(size_t)y*s->depth_pitch+x*4 : NULL;
        uint32_t z24=(uint32_t)(fmin(16777215,fmax(0,z))+.5);
        if(s->stencil_test) {
            uint8_t mask=(uint8_t)s->stencil_func_mask;
            if(!compare_value(s->stencil_func,(uint8_t)s->stencil_ref&mask,zp[0]&mask)) {
                stencil_update(s,zp,s->stencil_fail);continue;
            }
        }
        if (s->depth_test && !compare_value(s->depth_func,z24,read32(zp)>>8)) {
            if(s->stencil_test) stencil_update(s,zp,s->stencil_zfail);
            continue;
        }
        uint8_t *p=target+(size_t)y*s->target_pitch+x*s->target_bpp;
        if (s->blend) {
            float dst[4];
            if (s->target_bpp==2) unpack565(p[0]|(uint32_t)p[1]<<8,dst);
            else { dst[0]=p[2]/255.0f; dst[1]=p[1]/255.0f; dst[2]=p[0]/255.0f; dst[3]=p[3]/255.0f; }
            float source[4],destination[4];
            blend_factor(s->blend_src,rgb,dst,source);
            blend_factor(s->blend_dst,rgb,dst,destination);
            for (int k=0;k<4;++k) rgb[k]=rgb[k]*source[k]+dst[k]*destination[k];
        }
        if (s->dither && s->target_bpp==2) {
            /* Deterministic ordered approximation; NV2A's exact thresholds
             * still require hardware comparison. Preserve exact 565 levels. */
            static const unsigned bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
            float bias=((bayer[y&3][x&3]+.5f)/16.0f)-.5f;
            rgb[0]+=bias/31; rgb[1]+=bias/63; rgb[2]+=bias/31;
        }
        uint32_t pixel;
        if (s->target_bpp==2) pixel=quantize(rgb[0],31)<<11 | quantize(rgb[1],63)<<5 | quantize(rgb[2],31);
        else pixel=quantize(rgb[3],255)<<24 | quantize(rgb[0],255)<<16 | quantize(rgb[1],255)<<8 | quantize(rgb[2],255);
        for (unsigned k=0;k<s->target_bpp;++k) p[k]=(uint8_t)(pixel>>(8*k));
        if (zp && s->depth_write) { zp[1]=(uint8_t)z24; zp[2]=(uint8_t)(z24>>8); zp[3]=(uint8_t)(z24>>16); }
        if (s->stencil_test) stencil_update(s,zp,s->stencil_zpass);
    }
    return 1;
}

int nv2a_texture_copy_triangle(const NV2ATextureCopy *s,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    const float a[16][4], const float b[16][4], const float c[16][4])
{
    return nv2a_texture_copy_triangle_depth(s,texture,texture_size,target,target_size,
                                          NULL,0,a,b,c);
}
