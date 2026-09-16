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
#endif
