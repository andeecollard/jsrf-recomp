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

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("nv2a_metal_state: all translations agree\n");
    return 0;
}
