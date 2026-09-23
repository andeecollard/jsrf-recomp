/*
 * d3d8_ff_combiner.h -- host transcription of JSRF's (XDK 4134) D3D8
 * fixed-function texture-stage -> NV2A register-combiner builder.
 *
 * The guest function is at 0x00197F90 (D3D section). It is called from the
 * lazy state flusher at 0x001964A0 when bit 0x800 of the D3D dirty-flags word
 * 0x0019DED8 is set, with the device pointer as its only argument. See
 * experiments/d3d8_boundary/ff_combiner_notes.md for the full read of it.
 *
 * Everything here is PURE: no guest memory access. The caller supplies the
 * few guest values the function reads, in D3D8FFCombinerIn.
 *
 * Two more small pieces of D3D that write combiner registers for
 * fixed-function draws are transcribed beside it, because a host renderer
 * needs them too and 0x197F90 does not write them:
 *   - SetRenderState_TextureFactor (0x0018ECC0): FACTOR0[8] and FACTOR1[8].
 *   - the fog / final-combiner updater (0x00195610): SPECULAR_FOG_CW0/CW1.
 */
#ifndef D3D8_FF_COMBINER_H
#define D3D8_FF_COMBINER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XDK 4134 D3DTSS word indices used by the builder (per stage, 32 words). */
enum {
    D3D8FF_TSS_COLOROP   = 12,
    D3D8FF_TSS_COLORARG0 = 13,
    D3D8FF_TSS_COLORARG1 = 14,
    D3D8FF_TSS_COLORARG2 = 15,
    D3D8FF_TSS_ALPHAOP   = 16,
    D3D8FF_TSS_ALPHAARG0 = 17,
    D3D8FF_TSS_ALPHAARG1 = 18,
    D3D8FF_TSS_ALPHAARG2 = 19,
    D3D8FF_TSS_RESULTARG = 20
};

/* XDK 4134 D3D_g_RenderState indices the transcribed code reads
 * (0x0019E0E0 + 4*index). 4134 lacks 4432+/4627+ states, so these differ from
 * Cxbx-Reloaded's 5933-based X_D3DRS_* numbers (shown in brackets). */
enum {
    D3D8FF_RS_FOGENABLE          = 82,   /* 0x19E228 [5933: 92]  */
    D3D8FF_RS_SPECULARENABLE     = 93,   /* 0x19E254 [5933: 103] */
    D3D8FF_RS_POINTSPRITEENABLE  = 108,  /* 0x19E290 [5933: 118] */
    D3D8FF_RS_TEXTUREFACTOR      = 129   /* 0x19E2E4 [5933: 148] */
};

/* D3DTOP (XDK) */
enum {
    D3D8FF_TOP_DISABLE = 1, D3D8FF_TOP_SELECTARG1, D3D8FF_TOP_SELECTARG2,
    D3D8FF_TOP_MODULATE, D3D8FF_TOP_MODULATE2X, D3D8FF_TOP_MODULATE4X,
    D3D8FF_TOP_ADD, D3D8FF_TOP_ADDSIGNED, D3D8FF_TOP_ADDSIGNED2X,
    D3D8FF_TOP_SUBTRACT, D3D8FF_TOP_ADDSMOOTH,
    D3D8FF_TOP_BLENDDIFFUSEALPHA, D3D8FF_TOP_BLENDCURRENTALPHA,
    D3D8FF_TOP_BLENDTEXTUREALPHA, D3D8FF_TOP_BLENDFACTORALPHA,
    D3D8FF_TOP_BLENDTEXTUREALPHAPM, D3D8FF_TOP_PREMODULATE,
    D3D8FF_TOP_MODULATEALPHA_ADDCOLOR, D3D8FF_TOP_MODULATECOLOR_ADDALPHA,
    D3D8FF_TOP_MODULATEINVALPHA_ADDCOLOR, D3D8FF_TOP_MODULATEINVCOLOR_ADDALPHA,
    D3D8FF_TOP_DOTPRODUCT3, D3D8FF_TOP_MULTIPLYADD, D3D8FF_TOP_LERP,
    D3D8FF_TOP_BUMPENVMAP, D3D8FF_TOP_BUMPENVMAPLUMINANCE   /* = 26 */
};

/* D3DTA */
enum {
    D3D8FF_TA_DIFFUSE = 0, D3D8FF_TA_CURRENT = 1, D3D8FF_TA_TEXTURE = 2,
    D3D8FF_TA_TFACTOR = 3, D3D8FF_TA_SPECULAR = 4, D3D8FF_TA_TEMP = 5,
    D3D8FF_TA_COMPLEMENT = 0x10, D3D8FF_TA_ALPHAREPLICATE = 0x20
};

/* Bits of D3D8FFCombiners.unresolved: inputs on which the GUEST code indexes
 * past a jump table (it would branch into non-code). Nothing is guessed for
 * them; the affected slot is left as whatever the transcription had so far. */
#define D3D8FF_UNRES_BAD_OP   0x1u  /* COLOROP/ALPHAOP 0 or > 26            */
#define D3D8FF_UNRES_BAD_ARG  0x2u  /* (arg & 0xF) > 5 in a consumed arg     */

typedef struct D3D8FFCombinerIn {
    /* D3D_g_DeferredTextureState, 0x0019DEE0: 4 stages x 32 words. Only
     * words 12..20 of each stage are read. */
    uint32_t tss[4][32];
    /* D3D_g_RenderState[108] (0x19E290): nonzero -> start at stage 3. */
    uint32_t point_sprite_enable;
    /* D3D_g_RenderState[93] (0x19E254): gates SET_SPECULAR_ENABLE at the end. */
    uint32_t specular_enable;
    /* Bit s set iff the device's m_Textures[s] (device+0xA78+4s, absolute
     * 0x19BC78+4s) is non-NULL. Only nullness is read. */
    uint32_t texture_bound_mask;
    /* device+0x370 (the bound pixel shader). Nonzero: the guest returns at
     * once, writing nothing and leaving device_flags untouched. */
    uint32_t pixel_shader;
    /* device+8 (absolute 0x19B208) on entry. Only bit 0x40 ("some stage
     * reads D3DTA_SPECULAR") is read and written. */
    uint32_t device_flags;
} D3D8FFCombinerIn;

typedef struct D3D8FFCombiners {
    int      emitted;             /* 0 when pixel_shader != 0 (nothing written) */
    /* One pushbuffer packet each, in this order, as the guest writes them: */
    uint32_t combiner_control;    /* NV097_SET_COMBINER_CONTROL   0x1E60 (count only) */
    uint32_t color_icw[8];        /* NV097_SET_COMBINER_COLOR_ICW 0x0AC0 */
    uint32_t color_ocw[8];        /* NV097_SET_COMBINER_COLOR_OCW 0x1E40 */
    uint32_t alpha_icw[8];        /* NV097_SET_COMBINER_ALPHA_ICW 0x0260 */
    uint32_t alpha_ocw[8];        /* NV097_SET_COMBINER_ALPHA_OCW 0x0AA0 */
    /* Then, conditionally, NV097_SET_SPECULAR_ENABLE 0x03B8. */
    int      specular_enable_emitted;
    uint32_t specular_enable;
    uint32_t device_flags;        /* device+8 after the call */
    int      first_stage;         /* D3D stage feeding combiner 0: 0, or 3 for point sprites */
    uint32_t unresolved;          /* D3D8FF_UNRES_* */
} D3D8FFCombiners;

/* Transcription of guest 0x00197F90. Returns 0, or -1 if `unresolved` is
 * nonzero (the guest would have jumped through garbage). */
int d3d8_ff_combiners(const D3D8FFCombinerIn *in, D3D8FFCombiners *out);

/* Transcription of D3DDevice_SetRenderState_TextureFactor (0x0018ECC0):
 * when no pixel shader is bound it writes one 16-word packet at
 * NV097_SET_COMBINER_FACTOR0 (0x0A60), i.e. FACTOR0[0..7] then
 * FACTOR1[0..7], every word = tfactor. With a pixel shader bound it only
 * stores the render state and writes no registers (returns 0 then). */
int d3d8_ff_texture_factor(uint32_t tfactor, uint32_t pixel_shader,
                           uint32_t factor0[8], uint32_t factor1[8]);

/* The tail of the fog updater at 0x00195610 that writes the final combiner
 * (NV097_SET_COMBINER_SPECULAR_FOG_CW0/CW1, 0x0288/0x028C). The guest skips
 * these words when device+0x370 AND device+0x374 are both nonzero; returns 0
 * then. fog_enable = D3D_g_RenderState[82], specular_enable = [93]. */
int d3d8_ff_final_combiner(uint32_t fog_enable, uint32_t specular_enable,
                           uint32_t dev_370, uint32_t dev_374,
                           uint32_t *cw0, uint32_t *cw1);

#ifdef __cplusplus
}
#endif

#endif /* D3D8_FF_COMBINER_H */
