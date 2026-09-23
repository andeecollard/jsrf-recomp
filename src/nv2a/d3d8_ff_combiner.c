/*
 * d3d8_ff_combiner.c -- host transcription of guest 0x00197F90, the D3D8
 * (XDK 4134) fixed-function combiner builder in JSRF, plus the two small
 * register writers beside it. See d3d8_ff_combiner.h and
 * experiments/d3d8_boundary/ff_combiner_notes.md.
 *
 * This follows the guest instruction stream, quirks included. Guest
 * addresses are cited next to each piece so it can be re-checked against a
 * disassembly. Register roles in the guest:
 *   ebx = F      stage bits 0..1 | 0x10 first stage | 0xE8 in the alpha pass
 *   edi,esi,ebp = ARG0, ARG1, ARG2 of the op being built
 *   [esp+0x14]   the output control word being built (starts at `base`)
 */
#include "d3d8_ff_combiner.h"

#include <string.h>

/* Flag bits the guest keeps in ebx. */
#define F_STAGE_MASK 0x03u
#define F_FIRST      0x10u  /* this is the first combiner stage          */
#define F_ALPHA      0x20u  /* alpha pass: every input reads .a          */
#define F_DONE       0x80u  /* no (further) alpha pass for this stage    */
#define F_ALPHA_PASS 0xE8u  /* or'ed in at 0x1983B6: 0x80|0x40|0x20|0x08 */
#define F_OUT_OFS    0x48u  /* ebx & 0x48 = byte offset to the alpha arrays */

/* Device flag set by the input helper when an arg is D3DTA_SPECULAR. */
#define DEV_SPECULAR 0x40u

typedef struct {
    const D3D8FFCombinerIn *in;
    uint32_t dev_flags;
    uint32_t unresolved;
} Ctx;

/*
 * Guest 0x00197EF0: one combiner input byte, already shifted into its slot.
 *   arg (edi) : a D3DTA value
 *   ctl (ecx) : bits 16..19 = slot (3 A, 2 B, 1 C, 0 D), 0x10 = toggle
 *               complement, 0x20 = force alpha, 0x40 = EXPAND_NORMAL
 *   F   (edx) : the stage flags
 * Returns 0xFFFFFFFF << shift for D3DTA_TEXTURE on a stage with no texture;
 * the caller's 0xFF000000 test (0x19835E) catches that.
 */
static uint32_t ff_input(Ctx *c, uint32_t arg, uint32_t ctl, uint32_t F)
{
    uint32_t reg;
    switch (arg & 0xFu) {                     /* jump table at 0x197F74 */
    case D3D8FF_TA_DIFFUSE:  reg = 0x4; break;                 /* V0  0x197F43 */
    case D3D8FF_TA_CURRENT:  reg = (F & F_FIRST) ? 0x4 : 0xC;  /* V0 / R0 0x197F39 */
        break;
    case D3D8FF_TA_TEXTURE: {                                   /* 0x197EFD */
        uint32_t s = F & F_STAGE_MASK;
        reg = ((c->in->texture_bound_mask >> s) & 1u) ? s + 8u : 0xFFFFFFFFu;
        break;
    }
    case D3D8FF_TA_TFACTOR:  reg = 0x1; break;                 /* C0  0x197F17 */
    case D3D8FF_TA_SPECULAR:                                   /* V1  0x197F1E */
        c->dev_flags |= DEV_SPECULAR;
        reg = 0x5;
        break;
    case D3D8FF_TA_TEMP:     reg = 0xD; break;                 /* R1  0x197F32 */
    default:
        /* Indices 6..15 run off the six-entry table into the int3/nop
         * padding after it (0x197F8C reads 0x90909090). Not resolvable. */
        c->unresolved |= D3D8FF_UNRES_BAD_ARG;
        return 0;
    }
    /* 0x197F48..0x197F6D */
    uint32_t v = (((ctl | F | arg) >> 1) & 0x10u)   /* ALPHAREPLICATE / alpha pass -> .a */
               | (((ctl ^ arg) & 0x10u) << 1)       /* COMPLEMENT -> UNSIGNED_INVERT     */
               | (ctl & 0x40u)                      /* EXPAND_NORMAL                     */
               | reg;
    uint32_t shift = ((ctl >> 13) & 0x78u) & 31u;   /* x86 shl masks the count */
    return v << shift;
}

/* Slot selectors for ctl. */
#define SA 0x30000u
#define SB 0x20000u
#define SC 0x10000u
#define SD 0x00000u

int d3d8_ff_combiners(const D3D8FFCombinerIn *in, D3D8FFCombiners *out)
{
    memset(out, 0, sizeof(*out));
    out->device_flags = in->device_flags;

    /* 0x197F94: a bound pixel shader owns the combiners. */
    if (in->pixel_shader != 0)
        return 0;

    Ctx c = { in, in->device_flags & ~DEV_SPECULAR, 0 };
    const uint32_t old_flags = in->device_flags;       /* [esp+0x34] */
    out->emitted = 1;

    /* 0x197FB5: point sprites use texture stage 3 only. */
    const uint32_t start = in->point_sprite_enable ? 3u : 0u;
    out->first_stage = (int)start;

    uint32_t stage = start;          /* [esp+0x20] */
    uint32_t o = 0;                  /* output slot; [esp+0x18] pointer */
    uint32_t F = start | F_FIRST;
    uint32_t op = in->tss[stage][D3D8FF_TSS_COLOROP];

    for (;;) {
        const uint32_t *t = in->tss[stage];
        /* 0x19800D: RESULTARG == D3DTA_TEMP -> SUM_DST R1, else R0. */
        const uint32_t base = (t[D3D8FF_TSS_RESULTARG] == D3D8FF_TA_TEMP) ? 0xD00u : 0xC00u;
        uint32_t a0 = t[D3D8FF_TSS_COLORARG0];
        uint32_t a1 = t[D3D8FF_TSS_COLORARG1];
        uint32_t a2 = t[D3D8FF_TSS_COLORARG2];

        for (;;) {                   /* one pass: colour, then (usually) alpha */
            uint32_t ocw = base;     /* 0x198030 */
            uint32_t icw = 0;

            switch (op) {            /* jump table at 0x1984B4, index op-1 */
            case D3D8FF_TOP_DISABLE:                                   /* 0x19803E */
                if (F & F_FIRST) {
                    icw = ((F & F_ALPHA) << 23) | 0x04200000u;  /* V0 * 1 */
                    if ((F & F_ALPHA) == 0) {
                        /* Colour DISABLE on the first stage: one stage that
                         * passes diffuse colour AND diffuse alpha, whatever
                         * ALPHAOP says, then straight to the zero fill. */
                        out->color_icw[o] = icw;
                        out->color_ocw[o] = base;
                        out->alpha_icw[o] = icw | 0x10000000u;
                        out->alpha_ocw[o] = base;
                        o++;
                        goto fill;
                    }
                } else {
                    /* Later stage (only reachable as ALPHAOP): no-op stage. */
                    icw = 0;
                    ocw = 0;
                }
                break;

            case D3D8FF_TOP_SELECTARG1:                                /* 0x198088 */
                icw = ff_input(&c, a1, SA, F) | 0x00200000u;           /* A1 * 1 */
                break;
            case D3D8FF_TOP_BUMPENVMAP:                                /* 0x198292 */
            case D3D8FF_TOP_BUMPENVMAPLUMINANCE:
                icw = ff_input(&c, D3D8FF_TA_CURRENT, SA, F) | 0x00200000u;
                break;
            case D3D8FF_TOP_SELECTARG2:                                /* 0x19808F */
                icw = ff_input(&c, a2, SD, F) | 0x00002000u;           /* 1 * A2 (C*D) */
                break;

            case D3D8FF_TOP_MODULATE4X:  ocw |= 0x20000u; goto modulate; /* 0x1980A4 */
            case D3D8FF_TOP_MODULATE2X:  ocw |= 0x10000u; goto modulate; /* 0x1980C9 */
            case D3D8FF_TOP_MODULATE:                                    /* 0x1980D4 */
            modulate: {
                uint32_t x = ff_input(&c, a1, SA, F);
                icw = ff_input(&c, a2, SB, F) | x;                     /* A1 * A2 */
                break;
            }

            /* 0x1980EE ors 0x18000 then runs on into 0x1980F9's 0x8000. */
            case D3D8FF_TOP_ADDSIGNED2X: ocw |= 0x18000u; goto add;         /* 0x1980EE */
            case D3D8FF_TOP_ADDSIGNED:   ocw |= 0x08000u; goto add;         /* 0x1980F9 */
            case D3D8FF_TOP_ADD:                                             /* 0x198101 */
            add: {
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00202000u;    /* A1*1 + 1*A2 */
                icw = ff_input(&c, a2, SD, F) | x;
                break;
            }
            case D3D8FF_TOP_SUBTRACT: {                                /* 0x19811C */
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00204000u;    /* A1*1 + (-1)*A2 */
                icw = ff_input(&c, a2, SD, F) | x;
                break;
            }
            case D3D8FF_TOP_ADDSMOOTH: {                               /* 0x198137 */
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00200000u;
                x |= ff_input(&c, a1, SC | 0x10u, F);                  /* (1-A1) */
                icw = ff_input(&c, a2, SD, F) | x;                     /* A1 + (1-A1)*A2 */
                break;
            }
            case D3D8FF_TOP_BLENDDIFFUSEALPHA:                         /* 0x198163 */
            case D3D8FF_TOP_BLENDCURRENTALPHA:
            case D3D8FF_TOP_BLENDTEXTUREALPHA:
            case D3D8FF_TOP_BLENDFACTORALPHA: {
                /* op - 12 is the D3DTA of the blend factor: DIFFUSE,
                 * CURRENT, TEXTURE, TFACTOR. */
                uint32_t k = op - 12u;
                uint32_t x = ff_input(&c, a1, SA, F);
                x |= ff_input(&c, k, SB | 0x20u, F);                   /* k.a        */
                x |= ff_input(&c, k, SC | 0x30u, F);                   /* 1 - k.a    */
                icw = ff_input(&c, a2, SD, F) | x;                     /* A1*k.a + (1-k.a)*A2 */
                break;
            }
            case D3D8FF_TOP_BLENDTEXTUREALPHAPM: {                     /* 0x198194 */
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00200000u;
                x |= ff_input(&c, D3D8FF_TA_TEXTURE, SC | 0x30u, F);   /* 1 - T.a */
                icw = ff_input(&c, a2, SD, F) | x;
                break;
            }
            case D3D8FF_TOP_PREMODULATE: {                             /* 0x1981B9 */
                uint32_t x = ff_input(&c, a1, SA, F);
                if (F & F_FIRST)
                    icw = x | ff_input(&c, D3D8FF_TA_TEXTURE, SB, F);  /* A1 * T(this stage) */
                else
                    icw = x | 0x00200000u;                             /* A1 * 1 */
                break;
            }
            case D3D8FF_TOP_MODULATEALPHA_ADDCOLOR: {                  /* 0x1981F5 */
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00200000u;
                x |= ff_input(&c, a1, SC | 0x20u, F);                  /* A1.a */
                icw = ff_input(&c, a2, SD, F) | x;                     /* A1 + A1.a*A2 */
                break;
            }
            case D3D8FF_TOP_MODULATECOLOR_ADDALPHA:                    /* 0x198221 */
            case D3D8FF_TOP_MODULATEINVCOLOR_ADDALPHA: {               /* 0x198254 */
                uint32_t actl = (op == D3D8FF_TOP_MODULATECOLOR_ADDALPHA) ? SA : (SA | 0x10u);
                uint32_t x = ff_input(&c, a1, actl, F);                /* A1 or 1-A1 */
                x |= ff_input(&c, a2, SB, F);
                icw = ff_input(&c, a1, SC | 0x20u, F) | x | 0x20u;     /* ... + A1.a * 1 */
                break;
            }
            case D3D8FF_TOP_MODULATEINVALPHA_ADDCOLOR: {               /* 0x198228 */
                uint32_t x = ff_input(&c, a1, SA, F) | 0x00200000u;
                x |= ff_input(&c, a1, SC | 0x30u, F);                  /* 1 - A1.a */
                icw = ff_input(&c, a2, SD, F) | x;
                break;
            }
            case D3D8FF_TOP_DOTPRODUCT3: {                             /* 0x1982AD */
                uint32_t x = ff_input(&c, a1, SA | 0x40u, F);          /* expand(A1) */
                icw = ff_input(&c, a2, SB | 0x40u, F) | x;             /* . expand(A2) */
                /* AB_DST = the SUM_DST register, AB_DOT_ENABLE,
                 * AB_BLUE_TO_ALPHA; the alpha half of this slot is zeroed
                 * and the alpha pass skipped. */
                ocw = (base | 0x820000u) >> 4;
                out->alpha_icw[o] = 0;
                out->alpha_ocw[o] = 0;
                F |= F_DONE;
                break;
            }
            case D3D8FF_TOP_MULTIPLYADD: {                             /* 0x1982F2 */
                uint32_t x = ff_input(&c, a0, SA, F) | 0x00200000u;
                x |= ff_input(&c, a1, SC, F);
                icw = ff_input(&c, a2, SD, F) | x;                     /* A0 + A1*A2 */
                break;
            }
            case D3D8FF_TOP_LERP: {                                    /* 0x19831B */
                uint32_t x = ff_input(&c, a0, SA, F);
                x |= ff_input(&c, a1, SB, F);
                x |= ff_input(&c, a0, SC | 0x10u, F);                  /* 1 - A0 */
                icw = ff_input(&c, a2, SD, F) | x;                     /* A0*A1 + (1-A0)*A2 */
                break;
            }
            default:
                /* op 0 jumps through 0x1984B0 (-> 0x00498D00), op 27+
                 * through the padding after the table. Not resolvable. */
                c.unresolved |= D3D8FF_UNRES_BAD_OP;
                break;
            }

            /* 0x19835E: a missing texture made some slot 0xFF..; the whole
             * input word becomes "current (diffuse on the first stage) * 1".
             * The output word, scale and all, is kept. */
            if ((icw & 0xFF000000u) == 0xFF000000u)
                icw = (((~F) & F_FIRST) << 23) | 0x04200000u | ((F & F_ALPHA) << 23);

            if (F & F_OUT_OFS) {     /* 0x198399: ebx & 0x48 */
                out->alpha_icw[o] = icw;
                out->alpha_ocw[o] = ocw;
            } else {
                out->color_icw[o] = icw;
                out->color_ocw[o] = ocw;
            }

            if (F & F_DONE)          /* 0x1983A0: js */
                break;
            /* 0x1983A2: alpha pass of the same stage, same base. */
            F |= F_ALPHA_PASS;
            op = t[D3D8FF_TSS_ALPHAOP];
            a0 = t[D3D8FF_TSS_ALPHAARG0];
            a1 = t[D3D8FF_TSS_ALPHAARG1];
            a2 = t[D3D8FF_TSS_ALPHAARG2];
        }

        /* 0x1983C7: next stage, until 4 or a COLOROP of DISABLE. */
        o++;
        stage++;
        if (stage == 4)
            break;
        op = in->tss[stage][D3D8FF_TSS_COLOROP];
        if (op == D3D8FF_TOP_DISABLE)
            break;
        F = stage;
    }

fill:
    /* 0x1983FD: zero the unused slots; the stage count is COMBINER_CONTROL
     * (FACTOR0/1 "same for all stages", mux on LSB: bits 8..16 all 0). */
    for (uint32_t k = o; k < 8; k++) {
        out->color_icw[k] = out->color_ocw[k] = 0;
        out->alpha_icw[k] = out->alpha_ocw[k] = 0;
    }
    out->combiner_control = o;

    /* 0x19847A: turn vertex specular on/off to follow the combiners, unless
     * the title enabled it itself. */
    out->device_flags = c.dev_flags;
    if (((c.dev_flags ^ old_flags) & DEV_SPECULAR) && in->specular_enable == 0) {
        out->specular_enable_emitted = 1;
        out->specular_enable = (c.dev_flags >> 6) & 1u;
    }

    out->unresolved = c.unresolved;
    return c.unresolved ? -1 : 0;
}

int d3d8_ff_texture_factor(uint32_t tfactor, uint32_t pixel_shader,
                           uint32_t factor0[8], uint32_t factor1[8])
{
    if (pixel_shader != 0)   /* 0x18ECCF */
        return 0;
    for (int i = 0; i < 8; i++) {
        factor0[i] = tfactor;
        factor1[i] = tfactor;
    }
    return 1;
}

int d3d8_ff_final_combiner(uint32_t fog_enable, uint32_t specular_enable,
                           uint32_t dev_370, uint32_t dev_374,
                           uint32_t *cw0, uint32_t *cw1)
{
    if (dev_370 != 0 && dev_374 != 0)
        return 0;
    if (fog_enable) {
        /* 0x195750: A = fog.a, B = R0 (or V1+R0), C = fog.rgb, D = 0:
         * lerp(fog.rgb, colour, fog.a). */
        *cw0 = 0x130C0300u + (specular_enable ? 0x20000u : 0u);
    } else {
        /* 0x1957BC: D = R0, or V1+R0 with specular. */
        *cw0 = specular_enable ? 0xEu : 0xCu;
    }
    *cw1 = 0x1C80u;          /* G = R0.a, specular-add clamp */
    return 1;
}
