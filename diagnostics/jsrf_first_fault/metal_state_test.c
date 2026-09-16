/* Guest NV2A render state -> Metal enums.
 *
 * The interesting assertion here is not the table -- a table can be wrong in
 * exactly the way the test that copies it is wrong. It is the CROSS-CHECK
 * against nv2a_texture_copy_blend_factor_supported(), which is the set the
 * software rasteriser already implements and which this tree's own comment
 * calls the one place the three sinks must agree on. If the hardware path
 * cannot translate a factor that path renders, the two sinks have drifted and
 * that is a real defect, not a missing test case.
 *
 * No Metal, no device, no GPU: this is pure integer translation, which is why
 * it can be a ctest case at all. Everything else in the Metal backend needs a
 * device and lives in a script.
 */
#include "nv2a_metal_state.h"
#include "nv2a_texture_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_metal.h"

static int failures;

static void expect(int got, int want, const char *what, uint32_t guest)
{
    if (got == want) return;
    fprintf(stderr, "FAIL %s(0x%03X): got %d want %d\n", what, guest, got, want);
    ++failures;
}

int main(void)
{
    uint32_t f;

    /* Blend factors, against the encodings blend_factor() in
     * nv2a_texture_copy.c implements. */
    expect(nv2a_metal_blend_factor(0x000), NV2A_MTL_BLEND_ZERO, "blend", 0x000);
    expect(nv2a_metal_blend_factor(0x001), NV2A_MTL_BLEND_ONE, "blend", 0x001);
    expect(nv2a_metal_blend_factor(0x300), NV2A_MTL_BLEND_SRC_COLOR, "blend", 0x300);
    expect(nv2a_metal_blend_factor(0x301), NV2A_MTL_BLEND_ONE_MINUS_SRC_COLOR, "blend", 0x301);
    expect(nv2a_metal_blend_factor(0x302), NV2A_MTL_BLEND_SRC_ALPHA, "blend", 0x302);
    expect(nv2a_metal_blend_factor(0x303), NV2A_MTL_BLEND_ONE_MINUS_SRC_ALPHA, "blend", 0x303);
    expect(nv2a_metal_blend_factor(0x306), NV2A_MTL_BLEND_DST_COLOR, "blend", 0x306);
    expect(nv2a_metal_blend_factor(0x307), NV2A_MTL_BLEND_ONE_MINUS_DST_COLOR, "blend", 0x307);

    /* THE CROSS-CHECK. Every factor the software rasteriser accepts must have
     * a hardware translation, or the hardware path would refuse a draw the
     * existing path renders -- a regression the table above cannot show. */
    for (f = 0; f <= 0x400; ++f) {
        if (!nv2a_texture_copy_blend_factor_supported(f)) continue;
        if (nv2a_metal_blend_factor(f) < 0) {
            fprintf(stderr, "FAIL blend 0x%03X is rendered by the software "
                            "path and has no Metal translation\n", f);
            ++failures;
        }
    }

    /* Depth and stencil comparisons share one encoding; cmpf and cmpu in the
     * shader switch on the same constants. The guest value is 0x200 + the
     * Metal value, and asserting the relation as well as the entries catches a
     * transposed pair that individually-correct-looking cases would not. */
    for (f = 0x200; f <= 0x207; ++f)
        expect(nv2a_metal_compare_func(f), (int)(f - 0x200), "compare", f);

    /* Stencil operations, from stop() in the shader. 0 is ZERO and 0x1e00 is
     * KEEP -- the pair most likely to be conflated, so both are asserted. */
    expect(nv2a_metal_stencil_op(0x0000), NV2A_MTL_STENCIL_ZERO, "stencil", 0x0000);
    expect(nv2a_metal_stencil_op(0x1e00), NV2A_MTL_STENCIL_KEEP, "stencil", 0x1e00);
    expect(nv2a_metal_stencil_op(0x1e01), NV2A_MTL_STENCIL_REPLACE, "stencil", 0x1e01);
    expect(nv2a_metal_stencil_op(0x1e02), NV2A_MTL_STENCIL_INCR_CLAMP, "stencil", 0x1e02);
    expect(nv2a_metal_stencil_op(0x1e03), NV2A_MTL_STENCIL_DECR_CLAMP, "stencil", 0x1e03);
    expect(nv2a_metal_stencil_op(0x150a), NV2A_MTL_STENCIL_INVERT, "stencil", 0x150a);
    expect(nv2a_metal_stencil_op(0x8507), NV2A_MTL_STENCIL_INCR_WRAP, "stencil", 0x8507);
    expect(nv2a_metal_stencil_op(0x8508), NV2A_MTL_STENCIL_DECR_WRAP, "stencil", 0x8508);

    /* An unrecognised value must REFUSE, not default. A translator that
     * guesses renders the frame with the wrong equation and says nothing;
     * a refusal is a counter the caller can report. */
    expect(nv2a_metal_blend_factor(0x999), -1, "blend", 0x999);
    expect(nv2a_metal_compare_func(0x1FF), -1, "compare", 0x1FF);
    expect(nv2a_metal_compare_func(0x208), -1, "compare", 0x208);
    expect(nv2a_metal_stencil_op(0x1234), -1, "stencil", 0x1234);

    /* KEEP is 0 in Metal, so a caller writing `if (op)` to mean "translated"
     * would treat KEEP as a failure. Asserted so that the zero stays a
     * deliberate value rather than a trap. */
    if (NV2A_MTL_STENCIL_KEEP != 0) {
        fprintf(stderr, "FAIL MTLStencilOperationKeep is expected to be 0\n");
        ++failures;
    }

    /* EVERY HARDWARE DRAW MUST READ THE COLOUR ATTACHMENT.
     *
     * This is the invariant the 16 Sep 2026 fix rests on, and it is the only
     * part of that fix a test without a GPU can hold. The renderer chose its
     * fragment tail per draw, so opaque geometry -- most of a frame -- wrote
     * colour(0) with no destination read and no raster_order_group(0). At
     * gameplay that renders visibly corrupt on every frame.
     *
     * BE CLEAR ABOUT WHAT THIS DOES AND DOES NOT CATCH. It cannot see the
     * artefact: the artefact needs tens of thousands of draws with the GPU
     * behind the producer, which is exactly why every device gate in this tree
     * passed for weeks on a renderer a person could see was wrong. What it
     * catches is the DEFAULT being moved back, or the selector being rewritten
     * so that some draw class stops reading the destination -- which is how
     * the defect got in. The artefact itself is gated by the scripted ladder
     * in frame_bisect.sh; this is the cheap half.
     *
     * The modes below are control arms and are asserted to still reproduce the
     * old behaviours, because a defect you cannot switch back on is a defect
     * you cannot measure the cost of fixing. */
    {
        int blend, dither;
        for (blend = 0; blend <= 1; ++blend)
            for (dither = 0; dither <= 1; ++dither) {
                expect(nv2a_metal_shader_blend_for(3, blend, dither), 1,
                       "shader_blend mode 3", (uint32_t)(blend * 2 + dither));
                /* Anything past the named modes is the fix too, so a typo in
                 * an arm cannot quietly select the broken renderer. */
                expect(nv2a_metal_shader_blend_for(9, blend, dither), 1,
                       "shader_blend mode 9", (uint32_t)(blend * 2 + dither));
                expect(nv2a_metal_shader_blend_for(0, blend, dither), 0,
                       "shader_blend mode 0", (uint32_t)(blend * 2 + dither));
                expect(nv2a_metal_shader_blend_for(1, blend, dither),
                       blend && dither,
                       "shader_blend mode 1", (uint32_t)(blend * 2 + dither));
                expect(nv2a_metal_shader_blend_for(2, blend, dither), blend,
                       "shader_blend mode 2", (uint32_t)(blend * 2 + dither));
            }
        /* The default, read through the environment the way a run reads it.
         * The value matters less than that it answers 1 for every draw, which
         * is what the two lines below actually assert. */
        if (!getenv("RECOMP_METAL_SHADER_BLEND")) {
            int m = nv2a_metal_shader_blend_mode();
            expect(nv2a_metal_shader_blend_for(m, 0, 0), 1,
                   "default tail for an opaque undithered draw", (uint32_t)m);
            expect(nv2a_metal_shader_blend_for(m, 1, 1), 1,
                   "default tail for a blended dithered draw", (uint32_t)m);
        }
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("nv2a_metal_state: all translations agree,"
           " and every hardware draw reads the destination\n");
    return 0;
}
