/*
 * d3d8_ff_vertex_state.h -- host transcription of the D3D8 (XDK 4134) code in
 * JSRF that turns fixed-function vertex state into NV2A registers (G42):
 *
 *   texgen             SetTextureState_TexCoordIndex 0x0018F060 (immediate)
 *   texture transforms the lazy updater 0x001957F0 (dirty bit 0x400)
 *   lighting, material the lazy updater 0x00195F80 (dirty bit 0x1000) with its
 *                      helpers 0x001950A0, 0x00195CA0, 0x00195BA0
 *   fog                the lazy updater 0x00195610 (dirty bit 0x2000), the
 *                      part before the final-combiner tail G43 transcribed
 *   fog colour         SetRenderState_FogColor 0x0018EB80 (immediate)
 *
 * See experiments/d3d8_boundary/ff_lighting_fog_notes.md for the full read.
 *
 * Everything here is PURE: no guest memory access. The caller supplies the
 * guest values each routine reads. Float arithmetic follows the recompiled
 * guest: x87 code in double precision, rounded to float where the guest
 * stores a dword; SSE code in float, one operation per statement.
 */
#ifndef D3D8_FF_VERTEX_STATE_H
#define D3D8_FF_VERTEX_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XDK 4134 D3D_g_RenderState indices (0x0019E0E0 + 4*index) read here, each
 * checked against the dirty bit SetRenderState (0x18E960, table 0x1C4070)
 * gives it and the updater that reads it. Cxbx-Reloaded's 5933 numbers in
 * brackets: 10 higher up to SWAPFILTER, 19 higher from PSTEXTUREMODES on. */
enum {
    D3D8FF_RS_FOGTABLEMODE      = 83,   /* 0x19E22C [93]  dirty 0x2000 */
    D3D8FF_RS_FOGSTART          = 84,   /* 0x19E230 [94]  */
    D3D8FF_RS_FOGEND            = 85,   /* 0x19E234 [95]  */
    D3D8FF_RS_FOGDENSITY        = 86,   /* 0x19E238 [96]  */
    D3D8FF_RS_RANGEFOGENABLE    = 87,   /* 0x19E23C [97]  */
    D3D8FF_RS_LIGHTING          = 92,   /* 0x19E250 [102] dirty 0x1200 */
    D3D8FF_RS_LOCALVIEWER       = 94,   /* 0x19E258 [104] dirty 0x1000 */
    D3D8FF_RS_COLORVERTEX       = 95,   /* 0x19E25C [105] */
    D3D8FF_RS_BACKSPECULARMATERIALSOURCE = 96,   /* 96..103: the eight material sources, */
    D3D8FF_RS_EMISSIVEMATERIALSOURCE     = 103,  /* BACKSPECULAR .. EMISSIVE [106..113] */
    D3D8FF_RS_BACKAMBIENT       = 104,  /* 0x19E280 [114] */
    D3D8FF_RS_AMBIENT           = 105,  /* 0x19E284 [115] */
    D3D8FF_RS_FOGCOLOR          = 119,  /* 0x19E2BC [138] */
    D3D8FF_RS_TWOSIDEDLIGHTING  = 122,  /* 0x19E2C8 [141] */
    D3D8FF_RS_NORMALIZENORMALS  = 123   /* 0x19E2CC [142] */
};
/* XDK D3DTSS word indices read here (per stage, 32 words at 0x19DEE0). */
enum {
    D3D8FF_TSS_TEXTURETRANSFORMFLAGS = 21,
    D3D8FF_TSS_TEXCOORDINDEX         = 28
};

/* One register the transcription says D3D wrote. Values are raw words. */
typedef struct {
    uint16_t method;
    uint8_t  is_float;
    uint8_t  pad;
    uint32_t value;
} D3D8FFReg;

/* ---- texgen: SetTextureState_TexCoordIndex (0x0018F060) ------------------
 * Written immediately, not lazily: TEXGEN_S/T/R[stage] (0x03C0 + 0x10*stage,
 * one packet of three) all get the returned mode; TEXGEN_Q is not written.
 * *eye_normal gets the bit the guest keeps in device+0x450 (the mode needs
 * eye-space normals, so the transform updater emits the inverse model-view). */
uint32_t d3d8_ff_texgen(uint32_t texcoordindex, uint32_t *eye_normal);

/* ---- texture transforms: 0x001957F0 ------------------------------------- */
typedef struct {
    uint32_t vs_flags;          /* [device+0x380]+4; 0x12 set: the guest returns at once */
    uint32_t vs_tex_sizes;      /* [device+0x380]+0x10: components per texcoord set, a byte each */
    uint32_t ttf[4];            /* TSS[s][21] TEXTURETRANSFORMFLAGS */
    uint32_t tci[4];            /* TSS[s][28] TEXCOORDINDEX */
    uint32_t matrix[4][16];     /* device+0x7D0+0x40s: D3DTS_TEXTURE0+s as SetTransform stored it */
} D3D8FFTexXformIn;

#define D3D8FF_TX_CASES 9       /* A..H below, and "unresolved" */
typedef struct {
    int      emitted;           /* 0 when vs_flags & 0x12: nothing written */
    uint32_t enable[4];         /* TEXTURE_MATRIX_ENABLE[s], 0x0420 + 4s */
    uint32_t matrix_written;    /* bit s: TEXTURE_MATRIX[s] (0x06C0 + 0x40s, 16 words) written */
    uint32_t matrix[4][16];
    uint32_t key[4];            /* in << 8 | count << 4 | projected, when enabled */
    uint32_t tx_case[4];        /* 0..7 = the guest's eight layouts, 8 = unresolved */
    uint32_t unresolved;        /* bit s: the key reaches no valid case (the guest would jump to 0) */
} D3D8FFTexXform;
int d3d8_ff_tex_transforms(const D3D8FFTexXformIn *in, D3D8FFTexXform *out);

/* ---- fog: 0x00195610 up to the final-combiner tail ---------------------- */
typedef struct {
    uint32_t enable;            /* RS[82] FOGENABLE */
    uint32_t table_mode;        /* RS[83] */
    uint32_t start, end, density;   /* RS[84..86], float words */
    uint32_t range_enable;      /* RS[87] */
    uint32_t equal_scale;       /* the float word at 0x0019B0F4 (8192.0 in the XBE) */
} D3D8FFFogIn;
typedef struct {
    uint32_t enable;            /* FOG_ENABLE 0x02A4 */
    int      params_written;    /* 1: gen mode, mode and params were written too */
    uint32_t gen_mode;          /* FOG_GEN_MODE 0x02A0 */
    uint32_t mode;              /* FOG_MODE 0x029C */
    uint32_t params[3];         /* FOG_PARAMS 0x09C0, float words */
} D3D8FFFog;
void d3d8_ff_fog(const D3D8FFFogIn *in, D3D8FFFog *out);
/* SetRenderState_FogColor (0x0018EB80): FOG_COLOR 0x02A8, red and blue swapped. */
uint32_t d3d8_ff_fog_color(uint32_t d3dcolor);

/* ---- lighting and material: 0x00195F80 ---------------------------------- */
#define D3D8FF_LIGHT_WORDS 36   /* the device's 0x90-byte light record */
typedef struct {
    uint32_t vs_flags;          /* [device+0x380]+4 */
    uint32_t lighting;          /* RS[92] */
    uint32_t specular_enable;   /* RS[93] */
    uint32_t local_viewer;      /* RS[94] */
    uint32_t color_vertex;      /* RS[95] */
    uint32_t mat_source[8];     /* RS[96..103] */
    uint32_t back_ambient;      /* RS[104] */
    uint32_t ambient;           /* RS[105] */
    uint32_t two_sided;         /* RS[122] */
    uint32_t device_flags;      /* device+8 (bit 0x40: a combiner input reads SPECULAR) */
    uint32_t material[17];      /* device+0x9F0: D3DMATERIAL8 */
    uint32_t back_material[17]; /* device+0xA34 */
    uint32_t view[16];          /* device+0x750: D3DTS_VIEW */
    uint32_t eye[3];            /* the vector at 0x0019B0F8, (0, 0, -1) in the XBE */
    uint32_t list_head;         /* device+0x398, only zero or not */
    uint32_t nlights;           /* enabled lights in list order, at most 8 (the guest stops there) */
    uint32_t light[8][D3D8FF_LIGHT_WORDS];
} D3D8FFLightIn;

/* Bits of D3D8FFLights.unresolved. */
#define D3D8FF_LUNRES_SPECULAR  0x1u  /* 0x195E40 (SPECULAR_PARAMS 0x09E0, back 0x1E28) ran; not transcribed */
#define D3D8FF_LIGHT_REGS_MAX   300u
typedef struct {
    int      lit;               /* 1: the full path (lighting on, fixed-function) */
    uint32_t color_material;    /* COLOR_MATERIAL 0x0298 (0x001950A0) */
    uint32_t light_mask;        /* LIGHT_ENABLE_MASK 0x03BC */
    uint32_t unresolved;        /* D3D8FF_LUNRES_* */
    uint32_t types;             /* lights by kind: bits 0-7 directional, 8-15 point, 16-23 spot (counts) */
    unsigned n;
    D3D8FFReg reg[D3D8FF_LIGHT_REGS_MAX];  /* in emission order */
} D3D8FFLights;
int d3d8_ff_lights(const D3D8FFLightIn *in, D3D8FFLights *out);

/* The guest's float helpers, exposed for the unit test. */
double d3d8_ff_rsqrt(float x);                         /* 0x00190930 */
void   d3d8_ff_normalize(float v[3]);                  /* 0x001909E0 */
void   d3d8_ff_xform(float out[3], const float v[3], float w, const float m[16]);  /* 0x001906F0 */

#ifdef __cplusplus
}
#endif

#endif /* D3D8_FF_VERTEX_STATE_H */
