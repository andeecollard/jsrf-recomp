/* THE OUTPUT CONTROL WORD'S BIT LAYOUT, PINNED.
 *
 * This tree had two decoders for the same 32-bit word and they disagreed about
 * its first two fields. nv2a_texture_copy.c read bits 0..3 as the CD
 * destination and 4..7 as AB; d3d8_combiners.c read them the other way round.
 * Two handovers recorded the disagreement -- "d3d8_combiners.c:169 reads the
 * AB/CD destination bits inverted relative to nv2a_texture_copy.c ... Still
 * unfixed, Windows only" -- and neither resolved it, because nothing in the
 * suite touched d3d8_combiners.c at all.
 *
 * The authority both files name is xemu's parse_combiner_output:
 *
 *     out->cd        = value & 0xF;
 *     out->ab        = (value >> 4) & 0xF;
 *     out->muxsum    = (value >> 8) & 0xF;
 *     int flags      = value >> 12;
 *     out->cd_op     = flags & 1;      // bit 12
 *     out->ab_op     = flags & 2;      // bit 13
 *     out->muxsum_op = flags & 4;      // bit 14
 *     out->mapping   = flags & 0x38;   // bits 15..17
 *     out->ab_alphablue = flags & 0x80; // bit 19
 *     out->cd_alphablue = flags & 0x40; // bit 18
 *
 * CD IS THE LOW NIBBLE. This file asserts that against the D3D11 path's
 * parser, field by field, so the next person who has to choose between two
 * decoders is choosing between a decoder and a test.
 */
#include "../../src/d3d/d3d8_combiner_bits.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(cond, ...) do {                                           \
        if (!(cond)) { fail = 1;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }        \
    } while (0)

/* parse_output is static, so reach it the way the rest of the world does:
 * build a token + render-state array and read the parsed stage back. The
 * colour output word for stage 0 is render state index RS_COLOR_OCW0 below;
 * d3d8_combiners_parse_token is the only public entry. */
/* The header IS the decoder both paths now call, so the test needs nothing
 * else -- no D3D11, no device, no render-state array. That portability is the
 * point: d3d8_combiners.c is compiled only on Windows and includes <d3d11.h>
 * from its own header, so no test could reach its copy of these shifts, and
 * the inverted nibble survived two handovers that both named it. */
static void parse_one(uint32_t color_ocw, NV2AOutputWord *o)
{
    nv2a_parse_output_word(color_ocw, o);
}

int main(void)
{
    NV2AOutputWord o;

    /* ---- every field, one at a time, so a failure names the field ---- */
    parse_one(0x00000007u, &o);
    CHECK(o.cd_dst == 7, "bits 0..3 are the CD destination, got cd=%u ab=%u",
          o.cd_dst, o.ab_dst);
    CHECK(o.ab_dst == 0, "bits 0..3 must not reach ab_dst (got %u)", o.ab_dst);

    parse_one(0x00000070u, &o);
    CHECK(o.ab_dst == 7, "bits 4..7 are the AB destination, got ab=%u cd=%u",
          o.ab_dst, o.cd_dst);
    CHECK(o.cd_dst == 0, "bits 4..7 must not reach cd_dst (got %u)", o.cd_dst);

    parse_one(0x00000700u, &o);
    CHECK(o.sum_dst == 7, "bits 8..11 are SUM, got %u", o.sum_dst);

    parse_one(0x00001000u, &o);
    CHECK(o.cd_dot && !o.ab_dot, "bit 12 is the CD dot product");

    parse_one(0x00002000u, &o);
    CHECK(o.ab_dot && !o.cd_dot, "bit 13 is the AB dot product");

    parse_one(0x00004000u, &o);
    CHECK(o.mux_sum, "bit 14 is mux");

    parse_one(0x00038000u, &o);
    CHECK(o.output_map == 7, "bits 15..17 are the output mapping, got %u",
          o.output_map);

    parse_one(0x00040000u, &o);
    CHECK(o.cd_blue_to_alpha && !o.ab_blue_to_alpha,
          "bit 18 is CD blue-to-alpha");

    parse_one(0x00080000u, &o);
    CHECK(o.ab_blue_to_alpha && !o.cd_blue_to_alpha,
          "bit 19 is AB blue-to-alpha");

    parse_one(0x00100000u, &o);
    CHECK(o.unmodelled == 1, "bits 20+ are reported, not silently dropped");

    /* The alpha word has no dot and no blue-to-alpha. */
    nv2a_parse_alpha_output_word(0x000F3000u, &o);
    CHECK(!o.ab_dot && !o.cd_dot,
          "the alpha combiner has no dot product");
    CHECK(!o.ab_blue_to_alpha && !o.cd_blue_to_alpha,
          "the alpha combiner has no blue-to-alpha");
    CHECK(o.cd_dst == 0 && o.ab_dst == 0,
          "the alpha word's destinations still decode normally");

    /* ---- THE WORD THE TITLE ACTUALLY EMITS ----
     *
     * 0x000820D0 is JSRF's graffiti pixel shader, counted 24,821 times in one
     * player session. Under the inverted reading it decodes to ab_dst = 0 --
     * register ZERO, which the HLSL emitter treats as "discard" -- so the
     * tag's dot product was thrown away and the unused CD product was written
     * to R1 in its place. That is the case the bug actually cost, so it is the
     * case that is pinned. */
    parse_one(0x000820D0u, &o);
    CHECK(o.ab_dst == 13, "the graffiti shader writes AB to R1 (13), got %u",
          o.ab_dst);
    CHECK(o.cd_dst == 0, "its CD destination is ZERO/discard, got %u", o.cd_dst);
    CHECK(o.sum_dst == 0, "its SUM destination is discard, got %u", o.sum_dst);
    CHECK(o.ab_dot, "it uses a dot product for AB");
    CHECK(!o.cd_dot, "it does not dot CD");
    CHECK(o.ab_blue_to_alpha,
          "it moves the dot-product mask into alpha (bit 19)");
    CHECK(!o.cd_blue_to_alpha, "CD blue-to-alpha is clear");
    CHECK(!o.mux_sum, "it sums rather than muxes");
    CHECK(o.output_map == 0, "its output mapping is identity, got %u",
          o.output_map);

    /* AND THE THING THAT MADE THE BUG INVISIBLE. ab_dst == 0 is the emitter's
     * "discard" test, so the inverted reading did not produce a wrong colour
     * -- it produced NO WRITE AT ALL, and a register left at its initial value
     * still shades something plausible. A wrong picture gets reported; a
     * missing write gets lived with. */
    CHECK(o.ab_dst != 0,
          "ab_dst decoded as ZERO: the emitter would discard the tag's dot"
          " product entirely, which is the original defect");

    fprintf(stderr, "%s\n", fail ? "FAILED" : "ok");
    return fail;
}
