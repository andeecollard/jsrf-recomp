#include "nv2a_metal_state.h"

/* Blend factors. Guest encodings taken from blend_factor() in
 * nv2a_texture_copy.c and bfactor() in the Metal shader, which are the two
 * implementations already producing correct images.
 *
 * DST_ALPHA and ONE_MINUS_DST_ALPHA (0x304 / 0x305) are translated here and
 * are NOT in the software rasteriser's accepted set, because with depth packed
 * into alpha there is no destination alpha for it to read. On a real depth
 * attachment there is, so the hardware path can serve them -- which is one of
 * the things moving depth out of alpha buys. */
int nv2a_metal_blend_factor(uint32_t guest)
{
    switch (guest) {
    case 0x000: return NV2A_MTL_BLEND_ZERO;
    case 0x001: return NV2A_MTL_BLEND_ONE;
    case 0x300: return NV2A_MTL_BLEND_SRC_COLOR;
    case 0x301: return NV2A_MTL_BLEND_ONE_MINUS_SRC_COLOR;
    case 0x302: return NV2A_MTL_BLEND_SRC_ALPHA;
    case 0x303: return NV2A_MTL_BLEND_ONE_MINUS_SRC_ALPHA;
    case 0x304: return NV2A_MTL_BLEND_DST_ALPHA;
    case 0x305: return NV2A_MTL_BLEND_ONE_MINUS_DST_ALPHA;
    case 0x306: return NV2A_MTL_BLEND_DST_COLOR;
    case 0x307: return NV2A_MTL_BLEND_ONE_MINUS_DST_COLOR;
    case 0x308: return NV2A_MTL_BLEND_SRC_ALPHA_SATURATED;
    default:    return -1;
    }
}

/* Comparison functions, used for BOTH depth and stencil: the guest encodes
 * them identically and cmpf/cmpu in the shader switch on the same constants.
 *
 * The guest values are 0x200 + the Metal value, in the same order, so this
 * could be arithmetic. It is a switch because a table that happens to be
 * contiguous today is not a promise, and a bad value must return -1 rather
 * than index past the end. */
int nv2a_metal_compare_func(uint32_t guest)
{
    switch (guest) {
    case 0x200: return NV2A_MTL_CMP_NEVER;
    case 0x201: return NV2A_MTL_CMP_LESS;
    case 0x202: return NV2A_MTL_CMP_EQUAL;
    case 0x203: return NV2A_MTL_CMP_LESS_EQUAL;
    case 0x204: return NV2A_MTL_CMP_GREATER;
    case 0x205: return NV2A_MTL_CMP_NOT_EQUAL;
    case 0x206: return NV2A_MTL_CMP_GREATER_EQUAL;
    case 0x207: return NV2A_MTL_CMP_ALWAYS;
    default:    return -1;
    }
}

/* Stencil operations, from stop() in the shader.
 *
 * Note 0 is ZERO and 0x1e00 is KEEP -- they are different values and neither
 * is the other's default. The shader reaches KEEP through its `default` label,
 * so an unrecognised op there silently keeps; here it returns -1 and the
 * caller refuses the state. That is deliberate: "we do not know what this op
 * is" and "the guest asked to keep" are different facts. */
int nv2a_metal_stencil_op(uint32_t guest)
{
    switch (guest) {
    case 0x0000: return NV2A_MTL_STENCIL_ZERO;
    case 0x1e00: return NV2A_MTL_STENCIL_KEEP;
    case 0x1e01: return NV2A_MTL_STENCIL_REPLACE;
    case 0x1e02: return NV2A_MTL_STENCIL_INCR_CLAMP;
    case 0x1e03: return NV2A_MTL_STENCIL_DECR_CLAMP;
    case 0x150a: return NV2A_MTL_STENCIL_INVERT;
    case 0x8507: return NV2A_MTL_STENCIL_INCR_WRAP;
    case 0x8508: return NV2A_MTL_STENCIL_DECR_WRAP;
    default:     return -1;
    }
}
