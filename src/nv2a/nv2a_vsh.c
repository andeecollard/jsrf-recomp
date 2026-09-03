/* Portable NV2A decoder and vertex interpreter.
 * Encoding reference: xemu hw/xbox/nv2a/pgraph/glsl/vsh-prog.c,
 * field_mapping and decode_opcode (consulted 2026-09-03).
 * This implementation uses upload-order words, not an SDK instruction header.
 */
#include "nv2a_vsh.h"
#include <math.h>
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

static void mac_eval(NV2AVshMacOp op, float s[3][4], float out[4])
{
    const float *a = s[0], *b = s[1], *c = s[2];
    float dot = 0.0f;
    if (op == NV2A_VSH_MAC_DP3 || op == NV2A_VSH_MAC_DPH || op == NV2A_VSH_MAC_DP4) {
        for (int k = 0; k < (op == NV2A_VSH_MAC_DP4 ? 4 : 3); ++k) dot += a[k]*b[k];
        if (op == NV2A_VSH_MAC_DPH) dot += b[3];
    }
    for (int k = 0; k < 4; ++k) {
        switch (op) {
        case NV2A_VSH_MAC_MOV: out[k] = a[k]; break;
        case NV2A_VSH_MAC_MUL: out[k] = multiply(a[k], b[k]); break;
        case NV2A_VSH_MAC_ADD: out[k] = a[k] + c[k]; break;
        case NV2A_VSH_MAC_MAD: out[k] = multiply(a[k], b[k]) + c[k]; break;
        case NV2A_VSH_MAC_DP3: case NV2A_VSH_MAC_DPH: case NV2A_VSH_MAC_DP4: out[k] = dot; break;
        case NV2A_VSH_MAC_MIN: out[k] = fminf(a[k], b[k]); break;
        case NV2A_VSH_MAC_MAX: out[k] = fmaxf(a[k], b[k]); break;
        case NV2A_VSH_MAC_SLT: out[k] = a[k] < b[k]; break;
        case NV2A_VSH_MAC_SGE: out[k] = a[k] >= b[k]; break;
        default: out[k] = 0; break;
        }
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
        /* Read every operand before either execution unit writes anything. */
        for (int j = 0; j < 3; ++j)
            if ((used & (1u << j)) && !read_source(&s->mac_src[j], in, c, temp, out, address, inputs[j])) return 0;
        if (s->mac_dst.constant_reg >= 0 || s->ilu_dst.constant_reg >= 0) return 0;
        mac_eval(s->mac_op, inputs, m);
        ilu_eval(s->ilu_op, inputs[2], u);
        if (s->mac_op == NV2A_VSH_MAC_ARL) {
            float a = floorf(inputs[0][0]);
            if (!isfinite(a) || a < -2147483648.0f || a >= 2147483648.0f) return 0;
            address = (int)a;
        } else write_dest(&s->mac_dst, m, temp, out);
        write_dest(&s->ilu_dst, u, temp, out);
    }
    return 1;
}
