/* Build NV2A vertex-program words field by field, so a test can name the
 * instruction it means instead of pasting hex.
 *
 * This mirrors the field extraction in nv2a_vsh_parse (src/nv2a/nv2a_vsh.c).
 * It is a test fixture, not a second decoder: nothing in src/ uses it, and a
 * test that encodes with it must CHECK the parse result before believing any
 * output, or a drift in the decoder's bit positions would silently change what
 * the test is testing rather than failing it.
 *
 * Captured programs remain the better anchor where one exists (vsh_capture.h);
 * this exists for the opcodes no captured program happens to contain.
 */
#ifndef JSRF_VSH_ENCODE_H
#define JSRF_VSH_ENCODE_H
#include <stdint.h>

/* Swizzle byte: two bits per component, x in the high pair. */
#define SWZ(x,y,z,w) (unsigned)(((x)<<6)|((y)<<4)|((z)<<2)|(w))
#define SWZ_ID SWZ(0,1,2,3)

/* mux: 1 = temporary, 2 = input, 3 = constant. temp is the R# for mux 1. */
typedef struct { unsigned mux, temp, swz, neg; } VshSrc;

typedef struct {
    unsigned mac, ilu;          /* NV2AVshMacOp / NV2AVshIluOp */
    unsigned const_index;       /* shared c# for any source with mux 3 */
    unsigned input_index;       /* shared v# for any source with mux 2 */
    VshSrc a, b, c;             /* source A, B, C */
    unsigned mac_temp, mac_mask;/* MAC temporary destination */
    unsigned ilu_mask;          /* ILU temporary destination mask (temp is shared) */
    unsigned out_from_ilu;      /* 1 = the output register is written by the ILU */
    unsigned out_mask, out_reg; /* NV2AVshOutputReg */
    unsigned rel, final;
} VshIns;

static void vsh_encode(uint32_t *w, const VshIns *i)
{
    w[0] = 0;
    w[1] = ((i->ilu & 7u) << 25) | ((i->mac & 15u) << 21)
         | ((i->const_index & 255u) << 13) | ((i->input_index & 15u) << 9)
         | ((i->a.neg & 1u) << 8) | (i->a.swz & 255u);
    w[2] = ((i->a.temp & 15u) << 28) | ((i->a.mux & 3u) << 26)
         | ((i->b.neg & 1u) << 25) | ((i->b.swz & 255u) << 17)
         | ((i->b.temp & 15u) << 13) | ((i->b.mux & 3u) << 11)
         | ((i->c.neg & 1u) << 10) | ((i->c.swz & 255u) << 2)
         | ((i->c.temp >> 2) & 3u);
    w[3] = ((i->c.temp & 3u) << 30) | ((i->c.mux & 3u) << 28)
         | ((i->mac_mask & 15u) << 24) | ((i->mac_temp & 15u) << 20)
         | ((i->ilu_mask & 15u) << 16) | ((i->out_mask & 15u) << 12)
         | (1u << 11) /* destination is an output register, not a constant */
         | ((i->out_reg & 15u) << 3) | ((i->out_from_ilu & 1u) << 2)
         | ((i->rel & 1u) << 1) | (i->final & 1u);
}

#endif
