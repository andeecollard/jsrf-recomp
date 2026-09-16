/* MSL emission shared with the portable decoder tests.
 *
 * A line-for-line sibling of nv2a_vsh_hlsl.c: the same helper names, the same
 * order, the same structure, so the two files diff. Read that file first; this
 * comment only records where MSL forced a difference.
 *
 *   dot(a,b).xxxx        HLSL splats a scalar through a swizzle. MSL does not
 *                        allow swizzles on scalars, so every splat here is an
 *                        explicit float4(x) construction.
 *   asuint(x)            becomes as_type<uint>(x).
 *   : ATTR0 / SV_POSITION become [[attribute(0)]] / [[position]].
 *   cbuffer ... b1       becomes a `constant float4 *c [[buffer(1)]]` pointer
 *                        argument over the guest's 192 float4s. The HLSL
 *                        emitter indexes a0-relative reads as
 *                        c[(a0 + N) & 255] against a 192-entry array, which
 *                        is an out-of-bounds read for 192..255; in MSL, past
 *                        the end of a `constant` pointer, that is undefined
 *                        rather than merely clamped, so the index is clamped
 *                        here. It also keeps the buffer at 3072 bytes, inside
 *                        Metal's 4 KB setVertexBytes limit; 256 float4s would
 *                        be 4096 and would need a real MTLBuffer per batch.
 *                        The interpreter refuses the whole draw in this case
 *                        (read_source returns 0), which a vertex function
 *                        cannot do -- see the design note.
 *   VS_OUT               carries [[position]], and the NV2A screen-space to
 *                        clip-space conversion is done here, at the end of
 *                        main, because on the Metal path that conversion is
 *                        currently the entire body of the fixed `vs` in
 *                        nv2a_metal.m. A generated shader replacing the CPU
 *                        interpreter has to absorb it or the position it hands
 *                        the rasteriser is in the wrong space.
 *
 * STILL NOT VERIFIED AGAINST THE RENDERER -- nothing here has been run against
 * a real draw, and nothing in nv2a_metal.m calls it. What it HAS now been
 * checked against, as of 16 Sep 2026, is the CPU interpreter it is meant to
 * replace: diagnostics/jsrf_first_fault/vsh_msl_diff_test.m dispatches the
 * emitted code on a Metal device and diffs all sixteen output registers
 * against nv2a_vsh_execute, over the 126 vertex programs read out of the
 * title's own default.xbe plus one synthetic program per opcode. Two real
 * defects came out of that and are fixed here: fp contraction (see the pragma
 * in nv2a_vsh_generate_msl) and the multiply-by-zero rule (see MAC_MUL).
 * Read that file's header for what the result does and does not prove --
 * DPH, DST, EXP, LOG and LIT appear in no program this title ships, so they
 * are checked only against fixtures, and LIT is still knowingly wrong.
 */
#include "nv2a_vsh.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ================================================================
 * MSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index == 12)
            sb_append(sb, "R12"); /* oPos alias */
        else
            sb_append(sb, "R%d", src->reg_index);
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        /* The hardware wraps a0-relative indices at 8 bits before any bounds
         * check -- read_source in nv2a_vsh.c does the same -- so the mask is
         * part of the semantics, not a safety net. The clamp that follows it
         * is the safety net, and is a divergence; see the file comment. */
        if (src->rel_addr)
            sb_append(sb, "c[min((a0 + %d) & 255, %d)]",
                      src->reg_index, NV2A_VS_MAX_CONSTANTS - 1);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an MSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit a destination assignment (temp and/or output register write).
 *
 * The NV2A can write to both a temp register and an output register
 * simultaneously from the same operation. We emit two assignments
 * when both are active.
 *
 * @param dst     The destination operand
 * @param rhs     The MSL expression to assign (right-hand side)
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *rhs)
{
    /* Write to temp register if valid */
    if (dst->temp_reg >= 0 && dst->write_mask != 0) {
        if (dst->temp_reg == 12)
            sb_append(sb, "    R12");
        else
            sb_append(sb, "    R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }

    /* Write to output register if specified */
    if (dst->output_reg != NV2A_VSH_OUT_NONE && dst->output_mask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            if (dst->output_reg == NV2A_VSH_OUT_FOG) {
                /* Hardware broadcasts the first enabled component to fog.x;
                 * write_dest in nv2a_vsh.c does the same. */
                int k = 0;
                while (!(dst->output_mask & (8u >> k))) ++k;
                sb_append(sb, "    oFog.x = (%s).%c;\n", rhs, g_comp_names[k]);
                return;
            }
            sb_append(sb, "    %s", name);
            emit_write_mask(sb, dst->output_mask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, dst->output_mask);
            sb_append(sb, ";\n");
        }
    }
}

/**
 * Emit the MSL for one MAC operation.
 */
static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_source(&expr, &inst->mac_src[0], 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B, through vsh_mul, which carries NV2A's rule that a*0 and
         * 0*b are +0 even when the other operand is infinite or NaN --
         * nv2a_vsh.c's multiply().
         *
         * THIS USED TO BE A PLAIN `*`, and the comment here said the rule was
         * "NOT reproduced here, exactly as it is not reproduced by the HLSL
         * emitter". That was accurate and it was wrong, because the rule is
         * not an edge case in this title. JSRF's lighting shaders normalise a
         * vector the obvious way -- DP3 R0.x, R11, R11 / RSQ R1.x, R0.x /
         * MUL R2, R11, R1.x -- and a vertex whose normal is the zero vector
         * takes rsqrt(0) = +inf into that multiply. With `*` the GPU produces
         * NaN where the interpreter produces 0, and a NaN does not stay
         * local: it reaches oPos and the triangle disappears. Measured in
         * xbe@001F683C, where the NaN is then laundered back into a finite
         * but wrong value by a later MIN (min(NaN,x) returns x), so it does
         * not even announce itself as a NaN downstream.
         *
         * THE COST is two compares and a select per MUL, on the hottest path
         * there is, and it has NOT been measured against frame time. If it has
         * to go, delete the helper and put `*` back -- but then the CPU and
         * GPU paths are no longer the same function and the A/B that compares
         * them is measuring two things at once. */
        sb_append(&expr, "vsh_mul(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C. The product goes through vsh_mul for the same
         * reason MUL does; the interpreter's MAD is multiply() then +. */
        sb_append(&expr, "(vsh_mul(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ") + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated.
         * MSL has no scalar swizzle: splat with float4(). */
        sb_append(&expr, "float4(dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".xyz))");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "float4(dot(float4(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, 1.0f), ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "float4(dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w).
         * A PLAIN multiply, deliberately: mac_eval() computes DST in its own
         * block with `a[1]*b[1]` and does not route it through multiply(), so
         * matching the interpreter here means NOT applying the zero rule. */
        sb_append(&expr, "float4(1.0f, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".y * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".y, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".z, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a) */
        sb_append(&expr, "(1.0f - step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* a0 = floor(A.x) - special: writes address register, not a float reg */
        sb_append(sb, "    int next_address = int(floor(");
        emit_source(sb, &inst->mac_src[0], 1);
        sb_append(sb, "));\n");
        return; /* No destination register write */

    default:
        return;
    }

    sb_append(sb, "    float4 mac_result = %s;\n", expr_buf);
}

/**
 * Emit the MSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_source(&expr, &inst->ilu_src, 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = float4(1.0 / C.x) */
        sb_append(&expr, "float4(1.0f / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_RCC:
        sb_append(&expr, "vsh_rcc(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = float4(1.0 / sqrt(abs(C.x))) */
        sb_append(&expr, "float4(rsqrt(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")))");
        break;

    case NV2A_VSH_ILU_EXP:
    case NV2A_VSH_ILU_LOG:
        sb_append(&expr, inst->ilu_op == NV2A_VSH_ILU_EXP ? "vsh_exp(" : "vsh_log(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_LIT: {
        /* NV2A LIT instruction:
         *   dst.x = 1.0
         *   dst.y = max(src.x, 0.0)
         *   dst.z = (src.x > 0) ? pow(max(src.y,0), clamp(src.w, -B, B)) : 0
         *   dst.w = 1.0
         * with B = 127.99609375, which is 127 + 255/256 -- the interpreter's
         * bound, not 128.
         *
         * THIS WAS exp2(w * log2(max(y,0) + 1e-30)) WITH A BOUND OF 128, which
         * reads as the same function and is not. The epsilon exists to keep
         * log2 out of -inf, and it changes the answer wherever y <= 0: for a
         * negative y and a negative exponent the interpreter returns +inf
         * (powf(0, -n)) while that form returns about 5e25. 59 of 64 fixture
         * vectors disagreed, because half of a random y is negative. MSL's
         * pow() follows C here -- pow(+0, y<0) is +inf -- so it is simply the
         * right primitive, and the emitter now uses it.
         *
         * NOT CHECKED AGAINST THE TITLE: no program in JSRF's default.xbe
         * contains an ILU LIT, so the only evidence for this is the fixture in
         * vsh_msl_diff_test.m. Inline rather than a helper, matching the HLSL
         * emitter -- which still carries the old form. */
        sb_append(&expr, "float4(1.0f, max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x, 0.0f), (");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x > 0.0f) ? pow(max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".y, 0.0f), clamp(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w, -127.99609375f, 127.99609375f)) : 0.0f, 1.0f)");
        break;
    }

    default:
        return;
    }

    sb_append(sb, "    float4 ilu_result = %s;\n", expr_buf);
}

/**
 * Generate an MSL vertex function from a parsed NV2A program.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position        v1  = Blend weight     v2  = Normal
 *   v3  = Diffuse color   v4  = Specular color   v5  = Fog coordinate
 *   v6  = Point size      v7  = Back specular
 *   v8..v11 = Texture coords 0..3                v12-v15 = Additional
 *
 * Inputs arrive through [[stage_in]] with generic [[attribute(N)]] indices, so
 * an MTLVertexDescriptor can map any vertex buffer format at bind time -- the
 * MSL counterpart of the HLSL emitter's ATTR%d semantics.
 *
 * @return characters written, or 0 on error (invalid program, no FINAL,
 *         a constant-file write, or a truncated buffer).
 */
int nv2a_vsh_generate_msl(const NV2AVshProgram *program,
                          char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs;
    if (!program || !program->valid || !program->has_final || !buf || bufsize <= 0) return 0;
    for (i = 0; i < program->length; ++i)
        if (program->insns[i].mac_dst.constant_reg >= 0 || program->insns[i].ilu_dst.constant_reg >= 0) return 0;
    inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* CONTRACTION OFF, AND THIS IS NOT A STYLE CHOICE.
     *
     * MEASURED 16 Sep 2026 on an Apple M1 Max, mathMode = MTLMathModeSafe --
     * the mode nv2a_metal.m compiles its own shader in. The compiler still
     * fuses `a * b + c` into a single fma: for a = b = 1.0000001f and
     * c = -fl(a*b) it returns 1.42e-14 where separate rounding returns exactly
     * 0. Safe math and contraction are independent switches, and only this
     * pragma turns the second one off.
     *
     * It matters because MAD is emitted as `a * b + c` while the interpreter
     * computes it as multiply() then +, and multiply()'s zero test is a select
     * that blocks contraction on the CPU side. The two arithmetics then differ
     * by one rounding of the product -- normally invisible.
     *
     * It is NOT invisible here. NV2A's RCC saturates its reciprocal at 2^64, so
     * a skinning program that cancels an intermediate to exactly zero, divides,
     * and multiplies back -- which is what JSRF's do, `R10 = R3*(k - t) + R3*t`
     * then RCC of a dot product of R10 -- turns that one rounding into a
     * difference of order 1 in clip space. 51 of 8064 compared vectors across
     * the title's own 126 programs disagreed by up to 1.78 before this line,
     * and the disagreements were concentrated in seven programs, all of them
     * of exactly that shape. See diagnostics/jsrf_first_fault/vsh_msl_diff_test.m.
     *
     * The cost is real and unmeasured: the generated code loses fma on every
     * MAD. Measure it before deciding it is too expensive, and if it is, the
     * honest alternative is to fuse BOTH sides, not to leave them different. */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "#pragma clang fp contract(off)\n"
        "\n");

    /* Constants are a `constant float4 *` argument on main, declared below. */
    sb_append(&sb,
        "/* Constant file: %d float4 at buffer(1), %d bytes. a0-relative reads\n"
        " * wrap at 8 bits as the hardware does, then clamp to the last entry. */\n"
        "\n", NV2A_VS_MAX_CONSTANTS, NV2A_VS_MAX_CONSTANTS * 16);

    sb_append(&sb,
        "/* NV2A suppresses infinity and NaN through a multiply by zero: a*0 and\n"
        " * 0*b are +0 whatever the other operand is. nv2a_vsh.c's multiply()\n"
        " * does the same. It is not decoration -- see the note in\n"
        " * nv2a_vsh_msl.c beside MAC_MUL. */\n"
        "float4 vsh_mul(float4 a, float4 b) {\n"
        "    return select(a * b, float4(0.0f), (a == 0.0f) | (b == 0.0f)); }\n"
        "float4 vsh_rcc(float x) { float r = 1.0f / x;\n"
        "    float v = clamp(abs(r), 5.421010862427522e-20f, 1.8446744073709552e19f);\n"
        "    return float4((as_type<uint>(r) & 0x80000000u) != 0u ? -v : v); }\n"
        "float4 vsh_exp(float x) { return float4(exp2(floor(x)), x-floor(x), exp2(x), 1.0f); }\n"
        "float4 vsh_log(float x) { float t=log2(abs(x));\n"
        "    return float4(floor(t), x==0.0f ? 1.0f : abs(x)/exp2(floor(t)), t, 1.0f); }\n\n");

    /* Input structure - only declare used inputs.
     * An empty [[stage_in]] struct is not legal MSL, so a program that reads
     * no attributes gets no stage_in argument at all. */
    if (inputs) {
        sb_append(&sb, "struct VS_IN {\n");
        for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
            if (inputs & (1u << i)) {
                sb_append(&sb, "    float4 v%d [[attribute(%d)]];\n", i, i);
            }
        }
        sb_append(&sb, "};\n\n");
    }

    /* Viewport parameters for the NV2A screen-space to clip-space conversion
     * performed at the end of main. Field for field the values the fixed `vs`
     * in nv2a_metal.m reads out of its Params block. */
    sb_append(&sb,
        "struct VSH_Viewport {\n"
        "    float width;   /* NV097_SET_SURFACE_CLIP width  */\n"
        "    float height;  /* NV097_SET_SURFACE_CLIP height */\n"
        "    float depth;   /* z divisor, 16777215 for a 24-bit depth buffer */\n"
        "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos [[position]];\n"
        "    float4 oD0;\n"
        "    float4 oD1;\n"
        "    float4 oT0;\n"
        "    float4 oT1;\n"
        "    float4 oT2;\n"
        "    float4 oT3;\n"
        "    float  oFog;\n"
        "    float  oPts [[point_size]];\n"
        "    float4 oB0;\n"
        "    float4 oB1;\n"
        "};\n\n");

    /* Main function */
    if (inputs)
        sb_append(&sb, "vertex VS_OUT vsh_main(VS_IN input [[stage_in]],\n"
                       "                      constant float4 *c [[buffer(1)]],\n"
                       "                      constant VSH_Viewport &viewport [[buffer(2)]]) {\n");
    else
        sb_append(&sb, "vertex VS_OUT vsh_main(constant float4 *c [[buffer(1)]],\n"
                       "                      constant VSH_Viewport &viewport [[buffer(2)]]) {\n");

    /* Declare temporary registers R0-R11 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i < 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d = input.v%d;\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables. nv2a_vsh_execute seeds every output's w to 1
     * before the program runs; match that here or a program that never writes
     * oT0.w hands the rasteriser a zero it never had on hardware. */
    sb_append(&sb,
        "    /* Output registers (w seeded to 1, as nv2a_vsh_execute does) */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        sb_append(&sb, "\n    /* Instruction %d */\n", i);

        /* Evaluate both units before committing either destination. */
        sb_append(&sb, "    {\n");
        if (inst->mac_op != NV2A_VSH_MAC_NOP) emit_mac_op(&sb, inst);
        if (inst->ilu_op != NV2A_VSH_ILU_NOP) emit_ilu_op(&sb, inst);
        if (inst->mac_op == NV2A_VSH_MAC_ARL)
            sb_append(&sb, "    a0 = next_address;\n");
        else if (inst->mac_op != NV2A_VSH_MAC_NOP)
            emit_dest_assign(&sb, &inst->mac_dst, "mac_result");
        if (inst->ilu_op != NV2A_VSH_ILU_NOP)
            emit_dest_assign(&sb, &inst->ilu_dst, "ilu_result");
        sb_append(&sb, "    }\n");
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Populate output structure.
     *
     * Everything below this line is NOT part of the NV2A program. It is the
     * work the Metal path currently does on either side of the interpreter,
     * moved here because a generated vertex function has to do it all:
     *
     *   1. subpixel snap -- prepare_vertices in src/kernel/nv2a_pb_exec.c
     *      truncates screen x and y to 1/16 for values below 2^20;
     *   2. screen -> clip -- the fixed `vs` in src/nv2a/nv2a_metal.m divides by
     *      the surface clip size, flips y, normalises z by the depth range, and
     *      multiplies through by oPos.w because the guest program has already
     *      applied the perspective divide and left 1/w in oPos.w.
     *
     * Both are reproduced here by reading, not by measurement; see the design
     * note's list of what integration still has to verify. */
    sb_append(&sb,
        "    /* Screen-space fixup (see comment in nv2a_vsh_msl.c) */\n"
        "    float2 snapped = oPos.xy;\n"
        "    snapped.x = abs(snapped.x) < 1048576.0f ? trunc(snapped.x * 16.0f) / 16.0f : snapped.x;\n"
        "    snapped.y = abs(snapped.y) < 1048576.0f ? trunc(snapped.y * 16.0f) / 16.0f : snapped.y;\n"
        "    float z = oPos.z / viewport.depth;\n"
        "\n"
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    o.oPos = float4((snapped.x / viewport.width * 2.0f - 1.0f) * oPos.w,\n"
        "                    (1.0f - snapped.y / viewport.height * 2.0f) * oPos.w,\n"
        "                    z * oPos.w, oPos.w);\n"
        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.pos == sb.size - 1 ? 0 : sb.pos;
}
