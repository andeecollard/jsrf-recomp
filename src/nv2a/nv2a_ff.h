#ifndef NV2A_FF_H
#define NV2A_FF_H
#include <stdint.h>
/* Unlit, unskinned fixed-function subset. Matrices are method-register order.
 * Returns a reason for unsupported state; output uses viewport XYZ + clip W. */
const char *nv2a_ff_vertex(const uint32_t methods[2048],
                          const float input[16][4], float output[16][4]);
#endif
