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
#endif
