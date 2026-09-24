#ifndef NV2A_FF_H
#define NV2A_FF_H
#include <stdint.h>
/* Unlit, unskinned fixed-function subset. Matrices are method-register order.
 * Returns a reason for unsupported state; output uses viewport XYZ + clip W. */
const char *nv2a_ff_vertex(const uint32_t methods[2048],
                          const float input[16][4], float output[16][4]);
/* Degenerate normals met while NV097_SET_NORMALIZATION_ENABLE was set, split
 * by whether anything downstream would have read the value. `unread` draws
 * normally with a zero normal and is not a defect; `read` still rejects the
 * batch, because what hardware does there is not known. Read both before
 * touching that branch. */
extern unsigned long nv2a_ff_normal_unread, nv2a_ff_normal_read;
/* The method-seen table, or NULL.
 *
 * s_methods[] is zero-initialised, so this file cannot tell "never uploaded"
 * from "uploaded as zero" -- and the difference decides whether an enabled
 * texture matrix is a transform or an accident. The pushbuffer executor
 * already keeps the answer and already uses it for the composite matrix
 * (nv2a_pb_exec.c:2945). One assignment beside that guard,
 *
 *     nv2a_ff_method_seen = s_method_seen;
 *
 * arms every guard in nv2a_ff.c that needs it. Indexed the same way as the
 * method array: byte offset / 4, 0x2000/4 entries. Until it is set the guards
 * are inert and nv2a_ff_texmat_zero counts what they would have caught. */
extern const uint8_t *nv2a_ff_method_seen;
/* Fixed-function state that was silently wrong, or silently dropped, before
 * anything counted it. All of these print one line to stderr the first time
 * they fire, so a run shows them without a report line here; add them to the
 * [VSH] report when convenient.
 *
 *   texmat_unset          enabled texture matrix proved never uploaded (the
 *                         seen table said so): passed through untransformed
 *   texmat_zero           enabled texture matrix whose sixteen words are all
 *                         zero, provenance unknown: q becomes 0 and the sink
 *                         drops the unit's triangles unless
 *                         RECOMP_FF_TEXMAT_IDENTITY is set
 *   texcoord_nonfinite    the texture matrix multiply produced inf/NaN from
 *                         finite inputs -- nothing checked its output before
 *   clip_w_drawn          vertices with w=0 drawn instead of refusing the
 *                         whole batch (RECOMP_FF_CLIPW only)
 *   texgen_refused        batches refused for an unimplemented texgen mode;
 *                         the first line names the mode
 *   material_alpha_unset  lighting on with MATERIAL_ALPHA never uploaded,
 *                         which scaled every vertex alpha to zero
 *
 * All of them count VERTICES (nv2a_ff_vertex runs once per vertex, per
 * texture unit for the texmat three), not batches, so do not compare them
 * against s_vsh.rejected without dividing by the batch size.
 */
extern unsigned long nv2a_ff_texmat_unset, nv2a_ff_texmat_zero,
                     nv2a_ff_texcoord_nonfinite, nv2a_ff_clip_w_drawn,
                     nv2a_ff_texgen_refused, nv2a_ff_material_alpha_unset;

/* ================================================================
 * FIXED-FUNCTION T&L ON THE GPU.  RECOMP_METAL_FF, default OFF.
 * ================================================================
 *
 * WHY THIS EXISTS. nv2a_ff_vertex() runs on the CPU, once per vertex, for
 * every batch the guest draws without a programmable shader -- and that is
 * most of them. Measured in one gameplay run (idxfix3, [APU-VOICE] on=192..239
 * so gameplay by CLAUDE.md's own scene test):
 *
 *   [METAL] vsh draws: 508174 GPU, 707669 CPU     stderr.log:36987
 *   [GPU]   draws 1237171, 829095677 indices      stderr.log:36981
 *
 * and g_vsh_gpu_vertices says 308,800,552 of those 829 million index slots
 * were transformed on the GPU. The other 520 million ran through this file.
 *
 * WHAT IS SPLIT FROM WHAT. nv2a_ff_vertex branches on state and multiplies by
 * state, and only the branching half can be compiled into a shader. So:
 *
 *   NV2AFFKey        everything the function BRANCHES on. Small, POD, no
 *                    padding holes, hashed and compared as opaque bytes by
 *                    the backend's program cache. One emitted MSL function
 *                    per distinct key.
 *   nv2a_ff_constants  everything it MULTIPLIES by, as 192 float4 -- the same
 *                    shape and the same size as the programmable path's
 *                    constant file, so nv2a_metal.m's existing
 *                    `setVertexBytes:vsh_constants length:192*16 atIndex:1`
 *                    binds it with no change at all.
 *
 * THE CONVENTION THE EMITTED FUNCTION MUST PRODUCE is the one the sinks
 * already expect from s_outputs[i][0]: x and y in SCREEN PIXELS, z in
 * [0,16777215] post-viewport, and clip w UNDIVIDED. Measured, on a 640x480
 * surface, first fixed-function vertex of a run:
 *
 *   in=(4505.2212 -743.8711 0 1) -> out=(768.6722 150.8611 16785482 -2120.3469)
 *   render-investigation/ffdump1/stderr.log:6206
 *
 * so the composite matrix at 0x0680 ALREADY CARRIES THE VIEWPORT SCALE and
 * NV097_SET_VIEWPORT_SCALE must not be applied a second time. The same run
 * prints VPSCL=(320 -240 16777215 0) at ffdump1/stderr.log:6197, which is
 * exactly the factor a second application would multiply the screen by.
 *
 * WHAT IS DELIBERATELY NOT HERE. The subpixel snap. prepare_vertices()
 * truncates screen x and y to 1/16 only in its PROGRAMMABLE branch
 * (nv2a_pb_exec.c:2779); the fixed-function branch does not, so the emitted
 * fixed-function epilogue must not either, even though the programmable
 * emitter's epilogue does. Two paths, two tails, and they are not the same.
 */
#define NV2A_FF_KEY_VERSION 2u   /* 2: the fog byte (G53) */
typedef struct {
    uint8_t  version;        /* NV2A_FF_KEY_VERSION: an old cached shader for a
                              * new emitter is a wrong picture with no error */
    uint8_t  lighting;       /* NV097_SET_LIGHTING_ENABLE       0x0314 != 0 */
    uint8_t  normalise;      /* NV097_SET_NORMALIZATION_ENABLE  0x03A4 != 0 */
    uint8_t  normal_read;    /* lighting, or any NORMAL_MAP texgen component */
    uint8_t  texgen[4][4];   /* per unit, per component: 0 pass through,
                              * 1 NORMAL_MAP. Anything else refuses the key. */
    uint8_t  texmat[4];      /* per unit: 0 pass through, 1 transform. Folds
                              * NV097_SET_TEXTURE_MATRIX_ENABLE, the seen
                              * table and RECOMP_FF_TEXMAT_IDENTITY together,
                              * exactly as texture_matrix_usable() does. */
    uint8_t  lights;         /* bit L set: light L is INFINITE and enabled */
    uint8_t  fog;            /* G53: 0 FOG_ENABLE off (oFog stays 0), else the
                              * fog coordinate's source, NV2A_FF_FOG_*. */
    uint16_t inputs;         /* attribute mask, ascending, for [[attribute(n)]] */
    uint8_t  pad[4];         /* keep sizeof 32 and every byte defined */
} NV2AFFKey;

/* Where each piece of state lands in nv2a_ff_constants. Named here rather than
 * spelled twice, because the packer (nv2a_ff.c) and the emitter
 * (nv2a_vsh_msl.c) are in different files and a silent disagreement between
 * them is a black screen, not a compile error. */
enum {
    NV2A_FF_C_COMPOSITE = 0,   /*  4  composite matrix rows,  0x0680 */
    NV2A_FF_C_VIEWPORT  = 4,   /*  1  viewport offset x,y in .xy, 0x0A20 */
    NV2A_FF_C_NORMAL    = 5,   /*  4  inverse model-view rows, 0x0580 */
    NV2A_FF_C_TEXMAT    = 9,   /* 16  four texture matrices, 4 rows each, 0x06C0 */
    NV2A_FF_C_AMBIENT   = 25,  /*  1  scene ambient in .xyz,   0x0A10 */
    NV2A_FF_C_MATERIAL  = 26,  /*  1  emission in .xyz, material alpha in .w */
    NV2A_FF_C_LIGHT     = 27,  /* 24  per light L: ambient, diffuse, direction */
    NV2A_FF_C_FOGPLANE  = 51,  /*  1  NV097_SET_FOG_PLANE, 0x09D0 */
    NV2A_FF_C_MODELVIEW = 52,  /*  4  model-view rows, 0x0480 (eye position for fog) */
    NV2A_FF_C_USED      = 56,
    NV2A_FF_C_SLOTS     = 192  /* the programmable constant file's size, so the
                                * existing 3072-byte setVertexBytes binds it */
};
extern float nv2a_ff_constants[NV2A_FF_C_SLOTS][4];

/* G53: THE FIXED-FUNCTION FOG COORDINATE, per NV097_SET_FOG_GEN_MODE (0x02A0),
 * as xemu's fixed-function emitter forms it (pgraph/glsl/vsh-ff.c):
 *   0 SPEC_ALPHA   clamp(specular.a, 0, 1)     -- D3D's FOGTABLEMODE NONE
 *   1 RADIAL       |eye.xyz|                    -- D3D's RANGEFOGENABLE
 *   2 PLANAR       dot(plane.xyz, eye.xyz) + plane.w
 *   3 ABS_PLANAR   |PLANAR|
 *   6 FOG_X        the fog attribute (v5.x)
 * eye = the model-view matrix (0x0480) applied to the position, in matrix()'s
 * convention, which is the composite's. Written to output[5].x; nv2a_fog_factor
 * turns it into the factor per pixel. */
enum { NV2A_FF_FOG_OFF = 0, NV2A_FF_FOG_SPEC_ALPHA = 1, NV2A_FF_FOG_RADIAL = 2,
       NV2A_FF_FOG_PLANAR = 3, NV2A_FF_FOG_ABS_PLANAR = 4, NV2A_FF_FOG_X = 5 };
/* The key's fog byte for this state; -1 for a gen mode nobody models. */
int nv2a_ff_fog_source(const uint32_t methods[2048]);
/* The fog coordinate nv2a_ff_vertex writes to output[5].x, for the given
 * source, from the vertex's INPUTS (spec alpha from input 4, which the unlit
 * unit passes through unchanged). The eye position is returned in eye[4]
 * when the source forms one. For RECOMP_FOG_TRACE and the tests. */
float nv2a_ff_fog_coord(const uint32_t methods[2048], int source, const float in[16][4], float eye[4]);
/* Fog-coordinate census: vertices by source, and those with an unmodelled gen
 * mode (drawn with coordinate 0, i.e. the fog factor for distance zero). */
extern unsigned long nv2a_ff_fog_vertices[6], nv2a_ff_fog_unknown;

/* Can the GPU run this batch's fixed-function state, and what shape is it?
 *
 * Returns 1 and fills *key, or returns 0 and counts why. A 0 is a clean CPU
 * batch: the caller falls through to nv2a_ff_vertex exactly as before. The
 * accept set is a strict SUBSET of what nv2a_ff_vertex supports -- everything
 * it refuses per BATCH (skinning, local/spot lights, an unknown texgen mode)
 * is refused here too, and the one thing it refuses per VERTEX for a reason a
 * vertex function cannot express (a degenerate normal something reads) is
 * refused here as well unless RECOMP_FF_GPU_NORMAL_ZERO says otherwise.
 *
 * Reads nv2a_ff_method_seen, so the caller must have set it. */
int nv2a_ff_key(const uint32_t methods[2048], NV2AFFKey *key);
/* Pack the batch's float state into nv2a_ff_constants for the key just
 * returned. Separate from nv2a_ff_key because the key selects a SHADER and
 * this selects its ARGUMENTS: the matrices move every batch, the shape does
 * not. */
void nv2a_ff_params(const uint32_t methods[2048], const NV2AFFKey *key);
/* The MSL. Implemented in nv2a_vsh_msl.c beside the programmable emitter,
 * because the two have to agree on VS_IN, VS_OUT, VSH_Viewport and the exact
 * text of the vsh_main signature -- nv2a_metal.m's vsh_wrap() rewrites that
 * signature by strstr, and it is used unchanged for both.
 * Returns characters written, or 0. */
int nv2a_ff_generate_msl(const NV2AFFKey *key, char *buf, int bufsize);

/* BATCHES, not vertices -- unlike every other counter in this file, which is
 * why they are named apart rather than folded into the ones above. */
extern unsigned long nv2a_ff_gpu_backend_refused, nv2a_ff_gpu_clip_w;
/* Vertices the SINK would have dropped for q<=0 but the GPU path draws.
 * Counting only; see the definition. */
extern unsigned long nv2a_ff_gpu_texq_would_drop;
void nv2a_ff_count_texq(const NV2AFFKey *key, const float in[16][4],
                        unsigned texture_mask);
/* Per-vertex, for the GPU path: the composite w the shader will divide by.
 * 0 unless the vertex is safe to draw; see the definition. */
int nv2a_ff_clip_w_ok(const float pos[4]);
extern unsigned long nv2a_ff_gpu_batches, nv2a_ff_gpu_cpu_batches,
    nv2a_ff_gpu_no_skin, nv2a_ff_gpu_no_texgen, nv2a_ff_gpu_no_light,
    nv2a_ff_gpu_no_normal, nv2a_ff_gpu_texmat_unset, nv2a_ff_gpu_texmat_zero;
#endif
