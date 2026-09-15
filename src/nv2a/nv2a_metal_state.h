#ifndef NV2A_METAL_STATE_H
#define NV2A_METAL_STATE_H
#include <stdint.h>

/* Guest NV2A render state -> Metal pipeline and depth/stencil state.
 *
 * WHY THIS EXISTS, which is the whole point of the file.
 *
 * The Metal backend does the guest's blending, depth test and stencil BY HAND
 * IN THE FRAGMENT SHADER, reading both attachments back through
 * raster_order_group(0). That is not a stylistic choice, it is forced by one
 * decision made further up: 24-bit depth is packed into the colour
 * attachment's alpha channel (nv2a_metal.m, "24-bit depth in its alpha
 * channel, so there is no destination alpha"). With alpha spent on depth there
 * is no destination alpha to blend against, so hardware blending is
 * unavailable, so depth and stencil follow it into the shader, so every
 * overdrawn pixel serialises on the raster order group, so the attachment has
 * to be RGBA32Float -- 16 bytes per pixel for a surface the guest thinks is
 * two.
 *
 * Upstream's D3D11 backend does none of this. It keeps a real depth/stencil
 * buffer and uses hardware blending, and implements only the register
 * combiners in a pixel shader -- which is correct, because combiners really
 * are programmable and blending really is not.
 *
 * This header is the first piece of the same treatment for Metal: the pure
 * translation from the guest's enums to Metal's, with no Metal types and no
 * GPU, so it can be unit-tested rather than eyeballed. Getting one of these
 * mappings wrong produces a subtly wrong image in one blend mode, which is
 * exactly the class of bug that survives a screenshot.
 *
 * The guest encodings are not invented here. They are read off the shader and
 * the software rasteriser that already implement them correctly today:
 * `bfactor` and `blend_factor` for blending, `cmpf`/`cmpu` for comparisons,
 * `stop` for stencil operations. Those are the reference; if they and this
 * file ever disagree, they are right and this is wrong.
 */

/* Metal enum values, written as integers so this file needs no Metal headers
 * and the test needs no device. Static asserts in nv2a_metal.m check them
 * against the real MTL enums at compile time, so a Metal SDK change cannot
 * silently invalidate the table. */
/* Read off MTLRenderPipeline.h, not remembered. The first draft of this enum
 * had DestinationColor and DestinationAlpha transposed -- 6/7 against 8/9 --
 * which would have blended every DST_COLOR draw against the wrong channel and
 * shown up as a subtly wrong image in one blend mode. The _Static_asserts in
 * nv2a_metal.m caught it at compile time, which is the entire reason they are
 * there and the reason these are written out rather than computed. */
enum { NV2A_MTL_BLEND_ZERO = 0, NV2A_MTL_BLEND_ONE = 1,
       NV2A_MTL_BLEND_SRC_COLOR = 2, NV2A_MTL_BLEND_ONE_MINUS_SRC_COLOR = 3,
       NV2A_MTL_BLEND_SRC_ALPHA = 4, NV2A_MTL_BLEND_ONE_MINUS_SRC_ALPHA = 5,
       NV2A_MTL_BLEND_DST_COLOR = 6, NV2A_MTL_BLEND_ONE_MINUS_DST_COLOR = 7,
       NV2A_MTL_BLEND_DST_ALPHA = 8, NV2A_MTL_BLEND_ONE_MINUS_DST_ALPHA = 9,
       NV2A_MTL_BLEND_SRC_ALPHA_SATURATED = 10 };

enum { NV2A_MTL_CMP_NEVER = 0, NV2A_MTL_CMP_LESS = 1, NV2A_MTL_CMP_EQUAL = 2,
       NV2A_MTL_CMP_LESS_EQUAL = 3, NV2A_MTL_CMP_GREATER = 4,
       NV2A_MTL_CMP_NOT_EQUAL = 5, NV2A_MTL_CMP_GREATER_EQUAL = 6,
       NV2A_MTL_CMP_ALWAYS = 7 };

enum { NV2A_MTL_STENCIL_KEEP = 0, NV2A_MTL_STENCIL_ZERO = 1,
       NV2A_MTL_STENCIL_REPLACE = 2, NV2A_MTL_STENCIL_INCR_CLAMP = 3,
       NV2A_MTL_STENCIL_DECR_CLAMP = 4, NV2A_MTL_STENCIL_INVERT = 5,
       NV2A_MTL_STENCIL_INCR_WRAP = 6, NV2A_MTL_STENCIL_DECR_WRAP = 7 };

/* Every translator returns -1 for a value it does not recognise, and the
 * caller must fall back rather than substitute a default.
 *
 * A wrong blend factor draws the frame with the wrong equation and nothing
 * says so; a refusal is visible in a counter. This tree already learned the
 * lesson in the other direction -- a `fixed-function normal` rejection was
 * dropping 6% of all draws because an unusable value was treated as fatal
 * rather than as unused -- so: refuse the STATE, never silently guess it, and
 * never drop the draw for a value nothing reads. */
int nv2a_metal_blend_factor(uint32_t guest);
int nv2a_metal_compare_func(uint32_t guest);
int nv2a_metal_stencil_op(uint32_t guest);

/* NV097_SET_DEPTH_FUNC is 0 on a title that never wrote it, and the shader's
 * cmpf treats 0 as LEQUAL ("if(!f)f=0x203"). Kept here so the hardware path
 * and the shader path cannot disagree about an unwritten register. */
#define NV2A_GUEST_DEPTH_FUNC_DEFAULT 0x203u

#endif
