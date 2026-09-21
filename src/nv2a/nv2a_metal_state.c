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

/* WHICH HALF OF THE PIPELINE STILL HAS THE PICTURE. See the header for what
 * the four answers mean and why NOTHING_HELD is not a fifth spelling of black.
 *
 * ORDER IS THE WHOLE ALGORITHM, so it is written out rather than nested:
 *
 *  1. An entry whose texture was not read tells us nothing. If NO entry was
 *     read, say so and stop. Counting unread slots as black is the exact
 *     mistake this file's own header warns about.
 *  2. OWED outranks PICTURE_HELD. A frame that exists on the GPU and not in
 *     guest RAM is a lost write-back, and it stays the answer even when some
 *     other slot happens to be in both -- the frame going missing is the
 *     finding, not the one that made it.
 *  3. PICTURE_HELD needs pixels in BOTH. A texture with pixels whose guest
 *     count was not read is not evidence that the write-back worked, so it
 *     falls through to OWED only when guest RAM was read and came back zero;
 *     otherwise it is PICTURE_HELD on the texture alone, which is the weaker
 *     but still honest claim that rasterisation produced something.
 *  4. Only when every read texture is zero is the answer GPU_BLACK, and that
 *     is the one that says the draws themselves produced no pixels -- the
 *     fork that sends the next session to RECOMP_FB_DUMP_DRAW instead of to
 *     the write-back path. */
int nv2a_surface_census_verdict(const NV2ASurfaceCensus *entries, unsigned n)
{
    unsigned i, read = 0;
    int held = 0;

    if (!entries) return NV2A_SURFACE_CENSUS_NOTHING_HELD;
    for (i = 0; i < n; ++i) {
        if (entries[i].gpu_nonzero < 0) continue;
        ++read;
        if (entries[i].gpu_nonzero == 0) continue;
        if (entries[i].guest_nonzero == 0)
            return NV2A_SURFACE_CENSUS_OWED;
        held = 1;
    }
    if (!read) return NV2A_SURFACE_CENSUS_NOTHING_HELD;
    if (held)  return NV2A_SURFACE_CENSUS_PICTURE_HELD;
    return NV2A_SURFACE_CENSUS_GPU_BLACK;
}

const char *nv2a_surface_census_verdict_text(int verdict)
{
    switch (verdict) {
    case NV2A_SURFACE_CENSUS_GPU_BLACK:
        return "GPU-BLACK -- every surface the backend holds is black in its "
               "own texture too, so the draws produced no pixels; the loss is "
               "BEFORE the write-back (transform, shading or target), and the "
               "next measurement is RECOMP_FB_DUMP_DRAW inside one frame";
    case NV2A_SURFACE_CENSUS_OWED:
        return "OWED -- a surface holds pixels in its texture that guest RAM "
               "does not, so the frame was drawn and the write-back lost it; "
               "the loss is AFTER rasterisation and the target address on that "
               "line names the surface";
    case NV2A_SURFACE_CENSUS_PICTURE_HELD:
        return "PICTURE-HELD -- a surface has pixels in both its texture and "
               "guest RAM; if the screen is still black the loss is past this "
               "point, in the flip, the snapshot or the presenter";
    default:
        return "NOTHING-HELD -- no texture was read, so this census says "
               "nothing at all; it is not a black frame";
    }
}
