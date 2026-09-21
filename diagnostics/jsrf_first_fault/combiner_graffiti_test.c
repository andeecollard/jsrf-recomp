/*
 * The graffiti shader's combiner program must be ACCEPTED.
 *
 * Wall tags are a decal mesh drawn only in pass 6, sampling one 1024x1024
 * DXT3 atlas. DXT3 was always accepted and the texture formats are all
 * accepted since 21 Sep 2026 -- the tag never appeared because its PIXEL
 * SHADER was refused, and every draw that used it was dropped before either
 * backend saw it. A player session on 21 Sep counted 24,821 such refusals,
 * all on one output word.
 *
 * That word is 0x000820D0. Decoded against xemu's parse_combiner_output,
 * which is the reference this runtime's own header names:
 *
 *     bits  0..3  CD destination = 0    (discard)
 *     bits  4..7  AB destination = 13   (R1)
 *     bits  8..11 SUM destination = 0   (discard)
 *     bit  13     AB dot product
 *     bit  19     AB blue-to-alpha
 *
 * Three things had to be implemented before it could be accepted, and the
 * census proved all three live rather than assuming from the shader image:
 * the bits above 0xfff, destinations in the texture registers T0..T3
 * (the program writes 8 and 10 and reads them back), and nonzero per-stage
 * constants (0x0000FF00, 0x00FF0000, 0x000000FF -- pure channel masks, which
 * with a dot product is channel extraction).
 *
 * A REFUSED DRAW RENDERS NOTHING; a wrongly-shaded one renders something
 * plausible and wrong. These checks are what stop the second from being
 * mistaken for progress.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/nv2a/nv2a_texture_copy.h"
#include "texture_copy_state.h"

#define GRAFFITI_OCW 0x000820D0u

static uint32_t methods[2048];
static int failures;

static void check(const char *what, int ok, const char *detail)
{
    if (ok) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %-44s %s\n", what, detail ? detail : "");
    ++failures;
}

int main(void)
{
    NV2ATextureCopy s;
    const char *err;

    /* A minimal accepted program, then the graffiti features layered on, so a
     * failure names the feature that broke rather than "something". */
    copy_methods(methods, 3, 2, 8, 20, 4);
    modulate_methods(methods);
    err = nv2a_texture_copy_prepare(methods, &s);
    check("the baseline program is accepted", !err, err);

    /* 1. The output word itself -- the exact value the player's session
     *    refused 24,821 times. */
    methods[0x1e40/4] = GRAFFITI_OCW;
    err = nv2a_texture_copy_prepare(methods, &s);
    check("the graffiti output word is accepted", !err, err);
    check("...and is carried into the state",
          s.color_ocw[0] == GRAFFITI_OCW, "color_ocw[0] not preserved");

    /* 2. A destination in the texture registers. The program writes T0 and
     *    T2 and reads them back in a later stage. */
    methods[0x1e40/4] = 0x00000080u;          /* AB -> register 8 (T0) */
    err = nv2a_texture_copy_prepare(methods, &s);
    check("a texture-register destination is accepted", !err, err);
    methods[0x1e40/4] = 0x00000A00u;          /* SUM -> register 10 (T2) */
    err = nv2a_texture_copy_prepare(methods, &s);
    check("T2 as a sum destination is accepted", !err, err);

    /* 3. Nonzero per-stage constants, loaded rather than refused. */
    methods[0x1e40/4] = GRAFFITI_OCW;
    methods[0xa60/4]  = 0x0000FF00u;
    methods[0xa80/4]  = 0x000000FFu;
    err = nv2a_texture_copy_prepare(methods, &s);
    check("nonzero per-stage constants are accepted", !err, err);
    check("...C0 reaches the state", s.const0[0] == 0x0000FF00u, "const0 lost");
    check("...C1 reaches the state", s.const1[0] == 0x000000FFu, "const1 lost");

    /* THE OTHER DIRECTION. Widening must not become "accept anything": a
     * write to a constant register, and a bit above 19 that nothing models,
     * both still have to be refused. A gate that cannot refuse is not a gate,
     * and the first version of this widening accepted every destination
     * below 14 -- caught only because jsrf_texture_copy already asserted it. */
    methods[0xa60/4] = 0; methods[0xa80/4] = 0;
    methods[0x1e40/4] = 0x00000001u;          /* CD -> register 1 (a constant) */
    check("a write to a constant register is still refused",
          nv2a_texture_copy_prepare(methods, &s) != NULL,
          "register 1 is an input; writing it is meaningless");
    methods[0x1e40/4] = 0x00100000u;          /* bit 20: unmodelled */
    check("an unmodelled bit above 19 is still refused",
          nv2a_texture_copy_prepare(methods, &s) != NULL,
          "bit 20 has no meaning here and must not be shaded past");

    printf("combiner_graffiti_test: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
