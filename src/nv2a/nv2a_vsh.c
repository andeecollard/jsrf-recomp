/* Portable NV2A decoder and vertex interpreter.
 * Encoding reference: xemu hw/xbox/nv2a/pgraph/glsl/vsh-prog.c,
 * field_mapping and decode_opcode (consulted 2026-09-03).
 * This implementation uses upload-order words, not an SDK instruction header.
 */
#include "nv2a_vsh.h"
#include "../recomp_switch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

unsigned nv2a_vsh_mac_sources(NV2AVshMacOp op)
{
    switch (op) {
    case NV2A_VSH_MAC_NOP: return 0;
    case NV2A_VSH_MAC_MOV: case NV2A_VSH_MAC_ARL: return 1;
    case NV2A_VSH_MAC_ADD: return 5; /* A + C */
    case NV2A_VSH_MAC_MAD: return 7;
    default: return 3; /* A, B */
    }
}

static NV2AVshSrcOperand source(unsigned mux, unsigned temp, unsigned swz,
                               unsigned neg, int input, int constant, int rel)
{
    NV2AVshSrcOperand s;
    memset(&s, 0, sizeof(s));
    /* Hardware mux: 0 invalid, 1 temporary, 2 input, 3 constant. */
    s.reg_type = mux ? (NV2AVshRegType)(mux - 1) : NV2A_VSH_REG_COUNT;
    s.reg_index = mux == 2 ? input : mux == 3 ? constant : (int)temp;
    s.negate = neg;
    s.swizzle.x = (swz >> 6) & 3;
    s.swizzle.y = (swz >> 4) & 3;
    s.swizzle.z = (swz >> 2) & 3;
    s.swizzle.w = swz & 3;
    s.rel_addr = mux == 3 && rel;
    return s;
}

static int valid_source(const NV2AVshSrcOperand *s)
{
    switch (s->reg_type) {
    case NV2A_VSH_REG_TEMP: return s->reg_index < NV2A_VS_MAX_TEMPS;
    case NV2A_VSH_REG_INPUT: return s->reg_index < NV2A_VS_MAX_INPUTS;
    case NV2A_VSH_REG_CONST:
        /* Relative indices wrap at 8 bits before the bank bounds check. */
        return s->rel_addr || s->reg_index < NV2A_VS_MAX_CONSTANTS;
    default: return 0;
    }
}

static int output_reg_valid(unsigned n)
{
    return n == 0 || (n >= 3 && n <= 12);
}

int nv2a_vsh_parse(const uint32_t *words, int count, NV2AVshProgram *p)
{
    if (!p) return 0;
    memset(p, 0, sizeof(*p));
    if (!words || count <= 0 || count > NV2A_VS_MAX_INSTRUCTIONS) return 0;
    p->valid = 1;
    for (int i = 0; i < count; ++i) {
        const uint32_t w1 = words[i*4+1], w2 = words[i*4+2], w3 = words[i*4+3];
        NV2AVshInstruction *s = &p->insns[i];
        s->mac_op = (NV2AVshMacOp)((w1 >> 21) & 15);
        s->ilu_op = (NV2AVshIluOp)((w1 >> 25) & 7);
        s->const_index = (w1 >> 13) & 255;
        s->input_index = (w1 >> 9) & 15;
        s->is_final = w3 & 1;
        int rel = (w3 >> 1) & 1;
        s->mac_src[0] = source((w2 >> 26) & 3, w2 >> 28, w1 & 255,
                               (w1 >> 8) & 1, s->input_index, s->const_index, rel);
        s->mac_src[1] = source((w2 >> 11) & 3, (w2 >> 13) & 15, (w2 >> 17) & 255,
                               (w2 >> 25) & 1, s->input_index, s->const_index, rel);
        s->mac_src[2] = source((w3 >> 28) & 3, ((w2 & 3) << 2) | (w3 >> 30),
                               (w2 >> 2) & 255, (w2 >> 10) & 1,
                               s->input_index, s->const_index, rel);
        /* The shared C swizzle is scalarised for both units by scalar ILU ops. */
        if (s->ilu_op >= NV2A_VSH_ILU_RCP && s->ilu_op <= NV2A_VSH_ILU_LOG) {
            NV2AVshSwizzle *sw = &s->mac_src[2].swizzle;
            sw->y = sw->z = sw->w = sw->x;
        }
        s->ilu_src = s->mac_src[2];
        int paired = s->mac_op != NV2A_VSH_MAC_NOP && s->ilu_op != NV2A_VSH_ILU_NOP;
        s->mac_dst.temp_reg = (w3 >> 20) & 15;
        s->ilu_dst.temp_reg = paired ? 1 : s->mac_dst.temp_reg;
        s->mac_dst.write_mask = s->mac_op ? (w3 >> 24) & 15 : 0;
        s->ilu_dst.write_mask = s->ilu_op ? (w3 >> 16) & 15 : 0;
        if (paired && s->mac_dst.temp_reg == 1) s->mac_dst.write_mask = 0;
        if (s->mac_op == NV2A_VSH_MAC_ARL) s->mac_dst.write_mask = 0;
        s->mac_dst.output_reg = s->ilu_dst.output_reg = NV2A_VSH_OUT_NONE;
        s->mac_dst.constant_reg = s->ilu_dst.constant_reg = -1;
        NV2AVshDstOperand *out = (w3 & 4) ? &s->ilu_dst : &s->mac_dst;
        if (((w3 & 4) ? s->ilu_op != 0 : s->mac_op != 0)
                && !(out == &s->mac_dst && s->mac_op == NV2A_VSH_MAC_ARL)) {
            out->output_mask = (w3 >> 12) & 15;
            if (out->output_mask) {
                if (w3 & (1u << 11)) {
                    unsigned reg = (w3 >> 3) & 15;
                    out->output_reg = (NV2AVshOutputReg)reg;
                    if (!output_reg_valid(reg)) p->valid = 0;
                } else {
                    out->constant_reg = (w3 >> 3) & 255;
                    if (out->constant_reg >= NV2A_VS_MAX_CONSTANTS) p->valid = 0;
                }
            }
        }
        if ((s->mac_dst.write_mask && s->mac_dst.temp_reg >= NV2A_VS_MAX_TEMPS)
            || (s->ilu_dst.write_mask && s->ilu_dst.temp_reg >= NV2A_VS_MAX_TEMPS)
            || s->mac_op >= NV2A_VSH_MAC_COUNT) p->valid = 0;
        unsigned used = nv2a_vsh_mac_sources(s->mac_op);
        for (int j = 0; j < 3; ++j) {
            if (!(used & (1u << j)) && !(j == 2 && s->ilu_op)) continue;
            const NV2AVshSrcOperand *src = &s->mac_src[j];
            if (!valid_source(src)) p->valid = 0;
            if (src->reg_type == NV2A_VSH_REG_INPUT)
                p->inputs_read |= (uint16_t)(1u << src->reg_index);
        }
        p->length = i + 1;
        if (s->is_final) { p->has_final = 1; break; }
    }
    return p->valid;
}

static int read_source(const NV2AVshSrcOperand *s, const float in[16][4],
                       const float c[192][4], const float r[13][4],
                       const NV2AVshResult *out, int address, float v[4])
{
    const float *value;
    if (s->reg_type == NV2A_VSH_REG_INPUT) value = in[s->reg_index];
    else if (s->reg_type == NV2A_VSH_REG_TEMP)
        value = s->reg_index == 12 ? out->output[0] : r[s->reg_index];
    else if (s->reg_type == NV2A_VSH_REG_CONST) {
        unsigned idx = ((unsigned)s->reg_index + (s->rel_addr ? (unsigned)address : 0u)) & 255u;
        if (idx >= NV2A_VS_MAX_CONSTANTS) return 0;
        value = c[idx];
    } else return 0;
    const unsigned sw[4] = {s->swizzle.x, s->swizzle.y, s->swizzle.z, s->swizzle.w};
    for (int k = 0; k < 4; ++k) v[k] = s->negate ? -value[sw[k]] : value[sw[k]];
    return 1;
}

static float multiply(float a, float b)
{
    /* NV2A multiplication by zero also suppresses infinity/NaN. */
    return a == 0.0f || b == 0.0f ? 0.0f : a * b;
}

/* IS THE ZERO RULE ALSO THE DOT PRODUCT'S RULE? RECOMP_VSH_DP_ZERO=1 says yes.
 *
 * multiply() above is reached by MAC_MUL and MAC_MAD and by nothing else, so
 * the same zero that is suppressed through a MUL produces a NaN through a DP3
 * one slot later. ilu_eval's RSQ is 1/sqrt(|c.x|), which is +inf at c.x == 0
 * -- a vertex whose normal is the zero vector -- and 0 * inf is NaN wherever a
 * plain `*` gets hold of it.
 *
 * WHAT IT COSTS WHEN IT HAPPENS, and why it is not a cosmetic difference: a
 * NaN in a dot product that feeds a texture coordinate leaves oPos finite, so
 * the triangle is still drawn and then samples garbage. On an alpha-cutout
 * texture -- a wire fence -- the object disappears rather than going visibly
 * wrong, which is the failure mode that does not announce itself.
 *
 * WHY IT IS OFF, and why it stays off until a run says otherwise:
 *
 *   - [M] The dot products are the busiest MAC ops this title has. Over the
 *     126 distinct vertex programs in its own default.xbe: DP4 1248
 *     instructions, DP3 641, DPH 0. Turning this on puts a compare and a
 *     select on every component of every one of them, on the hottest path in
 *     the renderer, and that has NOT been measured against frame time.
 *   - [M] The population it can actually change is far smaller than that: 102
 *     of those 1889 dot products (46 DP3, 56 DP4) read an operand produced by
 *     an RCP, RCC or RSQ, which is where a non-finite operand comes from at
 *     all. The other 1787 pay the compare and cannot change an answer.
 *   - The RULE is read across from multiply(), which was itself verified
 *     against the MSL emitter and not against an NV2A. Nothing here has been
 *     compared with hardware.
 *
 * MAC_DST IS DELIBERATELY NOT COVERED. Its `a[1]*b[1]` below is a plain
 * multiply, and the note beside DST in nv2a_vsh_msl.c justifies that by saying
 * the emitter matches the interpreter. That is a MUTUAL-CONSISTENCY argument,
 * not evidence about hardware: it was equally true of these three dot products
 * until this switch existed, and it would have justified leaving them alone.
 * DST is out of scope here for a different and better reason -- [M] it occurs
 * zero times in the title's 126 programs, so no run can score it, while DP3
 * and DP4 occur 1889 times.
 *
 * THE MSL EMITTER READS THIS PREDICATE, not a second getenv of its own. The
 * interpreter and the generated shader are the two arms of the comparison in
 * diagnostics/jsrf_first_fault/vsh_msl_diff_test.m, and that comparison means
 * nothing if they land on opposite sides of a switch. Not declared in
 * nv2a_vsh.h: this is a switch, not API. */
static int g_vsh_dp_zero = -1;

int nv2a_vsh_dp_zero_on(void)
{
    if (g_vsh_dp_zero < 0) {
        /* DEFAULT ON since 16 Sep 2026. Evidence: 13 -> 0 mismatches over
         * the title's 126 programs in the GPU diff test, no frame cost
         * (57.3 -> 58.6 fps), a player confirming the fences on their own
         * build, and a scripted run whose log read "vsh_dp_zero OFF" while
         * the player watched the fences vanish in its window. Only the
         * literal RECOMP_VSH_DP_ZERO=0 turns it off, so an A/B is still one
         * token away and the state still prints below. */
        {   /* An EMPTY variable keeps the default: `VAR= cmd` is how a
             * shell unsets one for a command (recomp_switch.h), and a
             * harness that exports it empty must not turn the fence fix
             * off. Only a non-empty value that is not "0" or "0" itself
             * decides. */
            const char *e = getenv("RECOMP_VSH_DP_ZERO");
            g_vsh_dp_zero = (e && *e) ? recomp_switch_on("RECOMP_VSH_DP_ZERO") : 1;
        }
        /* recomp_switch.h: a switch that never names itself in a report cannot
         * be checked between the arms of an A/B, and three switches here were
         * believed for a while because nobody could. This file has no periodic
         * report to hang it on, so it says so once, in the shape
         * nv2a_pb_exec.c's [VSH-REUSE] line uses. */
        fprintf(stderr, "  [VSH] (vsh_dp_zero %s)\n", g_vsh_dp_zero ? "on" : "OFF");
        fflush(stderr);
    }
    return g_vsh_dp_zero;
}

static void mac_eval(NV2AVshMacOp op, float s[3][4], float out[4])
{
    const float *a = s[0], *b = s[1], *c = s[2];
    float dot = 0.0f;
    if (op == NV2A_VSH_MAC_DP3 || op == NV2A_VSH_MAC_DPH || op == NV2A_VSH_MAC_DP4) {
        const int n = op == NV2A_VSH_MAC_DP4 ? 4 : 3;
        /* The predicate is read once per dot product and hoisted out of the
         * component loop -- the same reason the opcode switch below is not
         * inside one. The OFF arm reads the cached variable directly rather
         * than calling through nv2a_vsh_dp_zero_on, so it pays a predictable
         * load and not a call per instruction per vertex. */
        const int zero = g_vsh_dp_zero < 0 ? nv2a_vsh_dp_zero_on() : g_vsh_dp_zero;
        if (zero) for (int k = 0; k < n; ++k) dot += multiply(a[k], b[k]);
        else      for (int k = 0; k < n; ++k) dot += a[k]*b[k];
        /* DPH's fourth term is 1 * b[3] and the rule cannot touch it: one
         * operand is never zero, and where b[3] is zero the product is +0
         * either way. It stays a bare add, so both arms agree here. */
        if (op == NV2A_VSH_MAC_DPH) dot += b[3];
    }
    /* Switch once, then run the component loop -- not the reverse. This used to
     * evaluate the opcode switch four times per instruction, once per
     * component, on the hottest path in the renderer. */
    switch (op) {
    case NV2A_VSH_MAC_MOV: for (int k=0;k<4;++k) out[k] = a[k]; break;
    case NV2A_VSH_MAC_MUL: for (int k=0;k<4;++k) out[k] = multiply(a[k], b[k]); break;
    case NV2A_VSH_MAC_ADD: for (int k=0;k<4;++k) out[k] = a[k] + c[k]; break;
    case NV2A_VSH_MAC_MAD: for (int k=0;k<4;++k) out[k] = multiply(a[k], b[k]) + c[k]; break;
    case NV2A_VSH_MAC_DP3: case NV2A_VSH_MAC_DPH: case NV2A_VSH_MAC_DP4:
        for (int k=0;k<4;++k) out[k] = dot; break;
    case NV2A_VSH_MAC_MIN: for (int k=0;k<4;++k) out[k] = fminf(a[k], b[k]); break;
    case NV2A_VSH_MAC_MAX: for (int k=0;k<4;++k) out[k] = fmaxf(a[k], b[k]); break;
    case NV2A_VSH_MAC_SLT: for (int k=0;k<4;++k) out[k] = a[k] < b[k]; break;
    case NV2A_VSH_MAC_SGE: for (int k=0;k<4;++k) out[k] = a[k] >= b[k]; break;
    default: for (int k=0;k<4;++k) out[k] = 0; break;
    }
    if (op == NV2A_VSH_MAC_DST) {
        out[0] = 1; out[1] = a[1]*b[1]; out[2] = a[2]; out[3] = b[3];
    }
}

static void ilu_eval(NV2AVshIluOp op, const float c[4], float out[4])
{
    float t = 0.0f;
    switch (op) {
    case NV2A_VSH_ILU_MOV: memcpy(out, c, 4*sizeof(float)); return;
    case NV2A_VSH_ILU_RCP: t = 1.0f / c[0]; break;
    case NV2A_VSH_ILU_RCC:
        t = 1.0f / c[0];
        t = copysignf(fminf(fmaxf(fabsf(t), 0x1p-64f), 0x1p64f), t); break;
    case NV2A_VSH_ILU_RSQ: t = 1.0f / sqrtf(fabsf(c[0])); break;
    case NV2A_VSH_ILU_EXP:
        out[0] = exp2f(floorf(c[0])); out[1] = c[0] - floorf(c[0]);
        out[2] = exp2f(c[0]); out[3] = 1; return;
    case NV2A_VSH_ILU_LOG:
        t = log2f(fabsf(c[0])); out[0] = floorf(t);
        out[1] = c[0] == 0 ? 1 : fabsf(c[0]) / exp2f(floorf(t));
        out[2] = t; out[3] = 1; return;
    case NV2A_VSH_ILU_LIT:
        out[0] = out[3] = 1; out[1] = fmaxf(c[0], 0);
        out[2] = c[0] > 0 ? powf(fmaxf(c[1], 0), fminf(fmaxf(c[3], -127.99609375f), 127.99609375f)) : 0;
        return;
    default: break;
    }
    for (int k = 0; k < 4; ++k) out[k] = t;
}

static void write_masked(float dst[4], const float value[4], unsigned mask)
{
    for (int k = 0; k < 4; ++k) if (mask & (8u >> k)) dst[k] = value[k];
}

static void write_dest(const NV2AVshDstOperand *dst, const float value[4],
                       float temp[13][4], NV2AVshResult *out)
{
    if (dst->write_mask) {
        if (dst->temp_reg == 12) {
            write_masked(out->output[0], value, dst->write_mask);
            out->written[0] |= dst->write_mask;
        } else write_masked(temp[dst->temp_reg], value, dst->write_mask);
    }
    if (dst->output_mask && dst->output_reg != NV2A_VSH_OUT_NONE) {
        float *d = out->output[dst->output_reg];
        if (dst->output_reg == NV2A_VSH_OUT_FOG) {
            /* Hardware broadcasts the first enabled component to fog.x. */
            for (int k = 0; k < 4; ++k) if (dst->output_mask & (8u >> k)) {
                d[0] = value[k]; break;
            }
        } else write_masked(d, value, dst->output_mask);
        out->written[dst->output_reg] |= dst->output_mask;
    }
}

int nv2a_vsh_execute(const NV2AVshProgram *p, const float in[16][4],
                     const float c[192][4], NV2AVshResult *out)
{
    float temp[13][4] = {{0}};
    int address = 0;
    if (!p || !p->valid || !p->has_final || !in || !c || !out) return 0;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 16; ++i) out->output[i][3] = 1;
    for (int i = 0; i < p->length; ++i) {
        const NV2AVshInstruction *s = &p->insns[i];
        float inputs[3][4] = {{0}}, m[4], u[4];
        unsigned used = nv2a_vsh_mac_sources(s->mac_op) | (s->ilu_op ? 4 : 0);
        /* Whether either unit's result can be observed at all. write_dest is a
         * no-op when both masks are clear, so computing a value nobody stores
         * is pure waste -- and a vertex program is mostly half-empty, because
         * an instruction word carries a MAC slot and an ILU slot and most
         * instructions use one of them. Every such slot used to cost a full
         * eval and a call into write_dest.
         *
         * ARL is the exception and must run whatever its masks say: its effect
         * is the address register, not a destination write. */
        int mac_stored = s->mac_dst.write_mask
                      || (s->mac_dst.output_mask
                          && s->mac_dst.output_reg != NV2A_VSH_OUT_NONE);
        int ilu_stored = s->ilu_dst.write_mask
                      || (s->ilu_dst.output_mask
                          && s->ilu_dst.output_reg != NV2A_VSH_OUT_NONE);
        /* Read every operand before either execution unit writes anything. */
        for (int j = 0; j < 3; ++j)
            if ((used & (1u << j)) && !read_source(&s->mac_src[j], in, c, temp, out, address, inputs[j])) return 0;
        if (s->mac_dst.constant_reg >= 0 || s->ilu_dst.constant_reg >= 0) return 0;
        if (s->mac_op == NV2A_VSH_MAC_ARL) {
            float a = floorf(inputs[0][0]);
            if (!isfinite(a) || a < -2147483648.0f || a >= 2147483648.0f) return 0;
            address = (int)a;
        } else if (mac_stored) {
            mac_eval(s->mac_op, inputs, m);
            write_dest(&s->mac_dst, m, temp, out);
        }
        if (ilu_stored) {
            ilu_eval(s->ilu_op, inputs[2], u);
            write_dest(&s->ilu_dst, u, temp, out);
        }
    }
    return 1;
}
