#ifndef D3D8_COMBINER_BITS_H
#define D3D8_COMBINER_BITS_H
/* THE NV2A COMBINER OUTPUT WORD, DECODED ONCE.
 *
 * This tree had two decoders for this 32-bit word and they disagreed about its
 * first two fields for at least two handovers:
 *
 *   nv2a_texture_copy.c   cd = w & 15,  ab = (w >> 4) & 15     (right)
 *   d3d8_combiners.c      ab = w & 15,  cd = (w >> 4) & 15     (inverted)
 *
 * Both files even said so in their comments -- "d3d8_combiners.c:169 reads the
 * AB/CD destination bits inverted relative to nv2a_texture_copy.c ... Still
 * unfixed, Windows only" -- and it stayed unfixed because the D3D11 file is
 * built only on Windows, includes <d3d11.h> at the top of its own header, and
 * therefore had no test anywhere.
 *
 * So the layout lives here instead: no includes beyond stdint, no Windows
 * types, no device. combiner_output_word_test.c pins every field against
 * xemu's parse_combiner_output on any host, which is the property that was
 * missing rather than the knowledge.
 *
 * THE AUTHORITY, quoted so the next reader does not have to find it --
 * xemu hw/xbox/nv2a/pgraph/glsl/psh.c:
 *
 *     static void parse_combiner_output(uint32_t value, struct OutputInfo *out)
 *     {
 *         out->cd = value & 0xF;
 *         out->ab = (value >> 4) & 0xF;
 *         out->muxsum = (value >> 8) & 0xF;
 *         int flags = value >> 12;
 *         out->cd_op = flags & 1;
 *         out->ab_op = flags & 2;
 *         out->muxsum_op = flags & 4;
 *         out->mapping = flags & 0x38;
 *         out->ab_alphablue = flags & 0x80;
 *         out->cd_alphablue = flags & 0x40;
 *     }
 *
 * CD IS THE LOW NIBBLE. Bits 20 and above are not assigned by any reference
 * this project has; nv2a_texture_copy.c refuses a program that sets one, and
 * the D3D11 path shades it as though it were clear. */
#include <stdint.h>

typedef struct NV2AOutputWord {
    unsigned cd_dst;           /* bits 0..3   */
    unsigned ab_dst;           /* bits 4..7   */
    unsigned sum_dst;          /* bits 8..11  */
    unsigned cd_dot;           /* bit 12      */
    unsigned ab_dot;           /* bit 13      */
    unsigned mux_sum;          /* bit 14      */
    unsigned output_map;       /* bits 15..17 */
    unsigned cd_blue_to_alpha; /* bit 18      */
    unsigned ab_blue_to_alpha; /* bit 19      */
    unsigned unmodelled;       /* bits 20..31, nonzero = we are guessing */
} NV2AOutputWord;

static inline void nv2a_parse_output_word(uint32_t w, NV2AOutputWord *o)
{
    o->cd_dst           = (w >>  0) & 0xFu;
    o->ab_dst           = (w >>  4) & 0xFu;
    o->sum_dst          = (w >>  8) & 0xFu;
    o->cd_dot           = (w >> 12) & 1u;
    o->ab_dot           = (w >> 13) & 1u;
    o->mux_sum          = (w >> 14) & 1u;
    o->output_map       = (w >> 15) & 7u;
    o->cd_blue_to_alpha = (w >> 18) & 1u;
    o->ab_blue_to_alpha = (w >> 19) & 1u;
    o->unmodelled       = (w >> 20);
}

/* The ALPHA output word is the same word with three fields that do not exist:
 * the alpha combiner has no dot product and no blue-to-alpha, so bits 12, 13,
 * 18 and 19 are not those things there. nv2a_texture_copy.c states this in
 * words -- "The alpha combiner writes only alpha, and has no dot or
 * blue-to-alpha" -- and reads only the destinations, mux and mapping from it.
 * Spelled as its own call so a caller cannot reach for the RGB one by
 * accident and light up a dot product on the alpha channel. */
static inline void nv2a_parse_alpha_output_word(uint32_t w, NV2AOutputWord *o)
{
    nv2a_parse_output_word(w, o);
    o->cd_dot = o->ab_dot = 0;
    o->cd_blue_to_alpha = o->ab_blue_to_alpha = 0;
}

#endif /* D3D8_COMBINER_BITS_H */
