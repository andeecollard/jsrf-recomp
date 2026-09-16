/* HLSL emission shared with the portable decoder tests.
 *
 * A line-for-line sibling of nv2a_vsh_msl.c: the same helper names, the same
 * order, the same structure, so the two files diff. Where a LANGUAGE forces a
 * difference -- splats, `f` suffixes, semantics, the constant file -- that
 * file's header lists it. Where the two disagree about ARITHMETIC, one of
 * them is wrong, and on 16 Sep 2026 three such disagreements were found.
 *
 * All three were found on the MSL side, by
 * diagnostics/jsrf_first_fault/vsh_msl_diff_test.m: it dispatches the emitted
 * shader on a Metal device and diffs all sixteen output registers against
 * nv2a_vsh_execute, over the 126 vertex programs read out of the title's own
 * default.xbe plus a fixture per opcode. All three were in this file too,
 * because the two files were written from each other. They are fixed here:
 *
 *   MAC_MUL / MAC_MAD        NV2A's a*0 = +0 rule, through vsh_mul()
 *   ILU_LIT                  pow() and the interpreter's 127.99609375 bound
 *   d3d8_vsh_generate_hlsl   contraction, which in HLSL is `precise`
 *
 * WHAT THAT EVIDENCE PROVES HERE, AND WHAT IT DOES NOT. The quoted numbers
 * were measured against the MSL emitter on an Apple M1 Max. What they are
 * about -- the decoded program, the NV2A rule, the interpreter it is being
 * matched to -- is shared between the two emitters, so the DIAGNOSIS carries
 * across. The fix does not: each language needs its own lever and the levers
 * behave differently, most of all for contraction. And nothing in this file
 * has ever been through a shader compiler. macOS has no fxc (see the platform
 * note in d3d8_vsh_generate_hlsl), so the gate on this file is textual --
 * hlsl_arithmetic_shape() in diagnostics/jsrf_first_fault/vsh_test.c -- and a
 * text gate cannot tell you what a compiler did with the text.
 *
 * STILL NOT VERIFIED AGAINST A RENDERER either: as on the MSL side, no draw
 * has ever used this. Three divergences from the MSL sibling that are NOT
 * fixed here are listed beside the code: the a0-relative clamp (emit_source),
 * the seeding of the output registers' w (d3d8_vsh_generate_hlsl) and the
 * absence of the screen-space fixup (the tail of the same function).
 */
#include "nv2a_vsh.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ================================================================
 * HLSL Code Generator
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
         * part of the semantics, not a safety net.
         *
         * DIVERGENCE FROM THE MSL SIBLING, LEFT ALONE. 192..255 index off the
         * end of a 192-entry cbuffer. That is not undefined in D3D11 the way
         * it is past the end of a Metal `constant` pointer -- an out-of-range
         * constant-buffer read returns zero -- which is why the MSL emitter
         * clamps and this one does not have to. It is still not what the
         * interpreter does: read_source REFUSES the draw for an index past the
         * constant file, and neither emitter can refuse. Nothing here has been
         * measured against a title program that indexes that high. */
        if (src->rel_addr)
            sb_append(sb, "c[(a0 + %d) & 255]", src->reg_index);
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
 * Map NV2A output register enum to an HLSL variable name.
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
 * @param prefix  The destination: temp register name
 * @param dst     The destination operand
 * @param rhs     The HLSL expression to assign (right-hand side)
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
 * Emit the HLSL for one MAC operation.
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
         * THIS USED TO BE A PLAIN `*`, and the MSL sibling's comment beside it
         * said the rule was "NOT reproduced here, exactly as it is not
         * reproduced by the HLSL emitter". That was an accurate description of
         * both files and a wrong description of the hardware, because the rule
         * is not an edge case in this title. JSRF's lighting programs
         * normalise a vector the obvious way -- DP3 R0.x, R11, R11 /
         * RSQ R1.x, R0.x / MUL R2, R11, R1.x -- and a vertex whose normal is
         * the zero vector takes rsqrt(0) = +inf into that multiply. With `*`
         * the GPU produces NaN where the interpreter produces 0, and a NaN
         * does not stay local: it reaches oPos and the triangle disappears.
         * MEASURED on the MSL path in the program at xbe@001F683C, where the
         * NaN is then laundered back into a finite but wrong value by a later
         * MIN (min(NaN,x) returns x), so it does not even announce itself as a
         * NaN downstream.
         *
         * That measurement is about the decoded program and the NV2A rule,
         * both of which this emitter shares, so it transfers unchanged. What
         * does NOT transfer is that the helper survives the optimiser: fxc is
         * licensed to delete exactly this test unless the value is declared
         * precise. See the contraction note in d3d8_vsh_generate_hlsl.
         *
         * THE COST is two compares and a select per MUL, on the hottest path
         * there is, and it has NOT been measured against frame time on either
         * path. If it has to go, delete the helper and put `*` back -- but
         * then the CPU and GPU paths are no longer the same function, and an
         * A/B between them is measuring two things at once. */
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
         * reason MUL does; the interpreter's MAD is multiply() then +. This is
         * also the instruction the contraction fix is about -- `precise` on
         * the result keeps the multiply and the add separately rounded, as
         * mac_eval's `multiply(a,b) + c` is. */
        sb_append(&expr, "(vsh_mul(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ") + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w).
         * A PLAIN multiply, deliberately, and it is the only one left in the
         * emitted body: mac_eval() computes DST in its own block with
         * `a[1]*b[1]` and does not route it through multiply(), so matching
         * the interpreter here means NOT applying the zero rule. */
        sb_append(&expr, "float4(1.0, ");
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
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
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
        sb_append(sb, "    int next_address = (int)floor(");
        emit_source(sb, &inst->mac_src[0], 1);
        sb_append(sb, ");\n");
        return; /* No destination register write */

    default:
        return;
    }

    /* `precise`, not decoration: it is this file's stand-in for the MSL
     * sibling's contraction pragma AND the thing that stops fxc folding
     * vsh_mul's zero test away. The argument is long and it is in
     * d3d8_vsh_generate_hlsl; do not remove this without reading it. */
    sb_append(sb, "    precise float4 mac_result = %s;\n", expr_buf);
}

/**
 * Emit the HLSL for one ILU operation.
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
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        sb_append(&expr, "vsh_rcc(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")).xxxx");
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
         * with B = 127.99609375, which is 127 + 255/256 -- ilu_eval's bound in
         * nv2a_vsh.c, not 128. The old bound was wrong by 0.00390625 for any
         * exponent outside +-128, which is a real difference in the exponent
         * of a pow and therefore not a small one.
         *
         * THE REAL DEFECT WAS THE EPSILON. This was
         * exp2(clamp(w,-128,128) * log2(max(y,0) + 1e-30)), which reads as the
         * same function and is not: the epsilon exists to keep log2 out of
         * -inf, and it changes the answer wherever y <= 0. For y <= 0 and a
         * negative exponent the interpreter returns +inf (powf(0,-n)) while
         * that form returns about 5e25. On the MSL side 59 of 64 fixture
         * vectors disagreed, because half of a random y is negative.
         *
         * WHY pow() IS STILL THE RIGHT PRIMITIVE HERE, where MSL got it for
         * free. Shader Model 5 assembly has no pow instruction, so fxc lowers
         * pow(x,y) to exp2(y * log2(x)) -- the same shape as the expression
         * above, minus the epsilon. Minus the epsilon is the point: log2(0) is
         * -inf, so x=0 with y<0 gives exp2(+inf) = +inf and x=0 with y>0 gives
         * exp2(-inf) = 0, both of which are what powf returns. The one case
         * that lowering gets wrong is 0^0, where 0 * -inf is NaN and powf
         * returns 1. MSL's pow follows C and needs no guard; this emitter adds
         * one on the exponent, which is exact because clamp(w) == 0 exactly
         * when w == 0. The guard returns 1 in precisely the case where powf
         * returns 1, so it is correct whether or not fxc lowers pow the way
         * the documentation says -- redundant at worst, never wrong. It is a
         * deliberate divergence from the MSL sibling's line.
         *
         * NOT CHECKED AGAINST THE TITLE, AND NOT AGAINST A COMPILER: no
         * program in JSRF's default.xbe contains an ILU LIT, so the only
         * execution evidence anywhere is the MSL fixture run, and no fxc has
         * ever seen this text. Inline rather than a helper, matching the MSL
         * emitter. */
        sb_append(&expr, "float4(1.0, max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x, 0.0), (");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x > 0.0) ? ((");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w == 0.0) ? 1.0 : pow(max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".y, 0.0), clamp(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w, -127.99609375, 127.99609375))) : 0.0, 1.0)");
        break;
    }

    default:
        return;
    }

    /* precise for the same reason mac_result is; see emit_mac_op. */
    sb_append(sb, "    precise float4 ilu_result = %s;\n", expr_buf);
}

/**
 * Map NV2A input register index to a D3D11 input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
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

    /* CONTRACTION OFF -- AND IN HLSL THAT IS `precise`, NOT A PRAGMA.
     *
     * The MSL sibling writes `#pragma clang fp contract(off)` at this point in
     * its preamble and explains why at length: the compiler fuses `a * b + c`
     * into one fma, nv2a_vsh_execute cannot, because multiply()'s zero test
     * makes it a select, and the two arithmetics then differ by one rounding
     * of the product. MEASURED there: 51 of 8064 compared vectors across the
     * title's own 126 programs disagreed by up to 1.78 ABSOLUTE IN CLIP SPACE,
     * concentrated in seven skinning programs of the shape
     * `R10 = R3*(k-t) + R3*t` followed by an RCC of a dot product of R10 --
     * an intermediate that cancels to exactly zero, divided, and multiplied
     * back, with RCC's 2^64 saturation turning the one rounding into an error
     * of order 1. See diagnostics/jsrf_first_fault/vsh_msl_diff_test.m.
     *
     * That measurement is about the arithmetic, and the arithmetic here is the
     * same arithmetic, so the diagnosis carries. The LEVER does not.
     *
     * HLSL HAS NO CONTRACTION PRAGMA. fxc -- which is what this text meets,
     * since src/d3d/d3d8_vsh.c calls D3DCompile(..., "vs_5_0",
     * D3DCOMPILE_OPTIMIZATION_LEVEL3) -- has three knobs and only one of them
     * lives in the shader text:
     *
     *   dcl_globalFlags refactoringAllowed  on by default in every recent
     *                                       shader model. It is what LICENSES
     *                                       the fusion, and it is not
     *                                       addressable from here.
     *   /Gis, D3DCOMPILE_IEEE_STRICTNESS    marks every value in the shader
     *                                       precise. A COMPILE FLAG: this file
     *                                       cannot set it, d3d8_vsh.c can, and
     *                                       that is the one-line alternative
     *                                       to everything below if the
     *                                       per-value form ever gets in the
     *                                       way.
     *   precise                             a modifier on a declaration, which
     *                                       propagates BACKWARDS to every
     *                                       value contributing to it. It is a
     *                                       shader-model-5 feature; if this
     *                                       ever has to compile as vs_4_0,
     *                                       the modifiers come out and /Gis
     *                                       goes in, or the fix is gone.
     *
     * AND FXC DOES FUSE, so this is not the case the brief allowed for where
     * the honest answer is to add nothing. Microsoft's HLSL team documented it
     * with the disassembly ("Precise and IEEE Strictness in HLSL",
     * learn.microsoft.com/en-us/archive/blogs/marcelolr): `float f = a;
     * f += b * c;` compiles to a single `mad`, and the same source with the
     * result declared precise compiles to `mul [precise(x)]` followed by
     * `add [precise]`. The D3D11.3 functional spec licenses it from the other
     * side -- a fused operation need only be "no less accurate than the worst
     * possible serial ordering of evaluation of the unfused expansion" -- and
     * every desktop GPU implements DXBC `mad` with its fused instruction.
     *
     * SO EVERY INSTRUCTION RESULT IS DECLARED precise; see the tail of
     * emit_mac_op and emit_ilu_op. One `precise` per NV2A instruction is
     * exactly the interpreter's grain: mac_eval and ilu_eval each write a
     * rounded float per component per instruction, and `precise` propagating
     * backwards from each result covers every arithmetic expression this
     * emitter generates -- every expression in the body is the initialiser of
     * a mac_result or an ilu_result.
     *
     * IT ALSO DOES A SECOND JOB THE MSL PRAGMA DOES NOT HAVE TO DO. The same
     * refactoringAllowed flag lets fxc ignore NaN and INF behaviour, and
     * vsh_mul below is nothing BUT a test that only matters when the other
     * operand is INF or NaN. Under that licence `(a==0||b==0) ? 0 : a*b`
     * simplifies to `a*b` and the zero rule is optimised away silently at
     * optimisation level 3. The same blog post's first example is exactly this
     * shape: `0 * a` folds to `mov l(0)` without precise and survives as a
     * real `mul [precise]` with it.
     *
     * NOT MEASURED HERE, AND IT CANNOT BE ON THIS HOST. fxc is
     * d3dcompiler_47.dll; running a Windows x86-64 binary on this machine
     * needs Rosetta 2, which is not installed -- CrossOver's wineloader and
     * Whisky's wine64 are themselves x86-64 Mach-O and will not start ("bad
     * CPU type in executable"). So the evidence for this fix is the
     * documentation above plus the MSL measurement of the underlying
     * arithmetic, and the gate is hlsl_arithmetic_shape() in
     * diagnostics/jsrf_first_fault/vsh_test.c, which asserts the modifier is
     * on every instruction result and nothing whatever about what fxc then
     * does with it. THE MEASUREMENT THIS COMMENT IS STANDING IN FOR is one
     * fxc run on Windows: compile any emitted program and grep the
     * disassembly for `mad`. There should be none. */

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "\n", NV2A_VS_MAX_CONSTANTS);

    /* vsh_mul is the a*0 rule; see MAC_MUL. The vector `||` and `?:` here are
     * fxc's component-wise forms, which is what D3DCompile("vs_5_0") gives.
     * HLSL 2021 under dxc removes both on vectors -- they become or() and
     * select() -- so a move to dxc has to rewrite this line, and the compiler
     * will say so rather than miscompile it. */
    sb_append(&sb,
        "/* NV2A suppresses infinity and NaN through a multiply by zero: a*0\n"
        " * and 0*b are +0 whatever the other operand is. nv2a_vsh.c's\n"
        " * multiply() does the same. It is not decoration -- see the note\n"
        " * beside MAC_MUL in nv2a_vsh_hlsl.c. */\n"
        "float4 vsh_mul(float4 a, float4 b) {\n"
        "    return (a == 0.0 || b == 0.0) ? float4(0,0,0,0) : a * b; }\n");

    sb_append(&sb,
        "float4 vsh_rcc(float x) { float r = 1.0 / x;\n"
        "    float v = clamp(abs(r), 5.421010862427522e-20, 1.8446744073709552e19);\n"
        "    return ((asuint(r) & 0x80000000u) ? -v : v).xxxx; }\n"
        "float4 vsh_exp(float x) { return float4(exp2(floor(x)), x-floor(x), exp2(x), 1); }\n"
        "float4 vsh_log(float x) { float t=log2(abs(x));\n"
        "    return float4(floor(t), x==0 ? 1 : abs(x)/exp2(floor(t)), t, 1); }\n\n");

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
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

    /* Output register variables.
     *
     * DIVERGENCE FROM THE MSL SIBLING AND FROM THE INTERPRETER, LEFT ALONE
     * AND REPORTED. nv2a_vsh_execute seeds EVERY output register's w to 1
     * before the program runs (`for (i=0;i<16;++i) out->output[i][3] = 1;`),
     * and the MSL emitter matches it; this file seeds w=1 for oPos, oD0 and
     * oD1 only, and zero for the other eight. A program that never writes
     * oT0.w therefore hands the rasteriser 0 here and 1 there. It is a
     * one-word fix and it is deliberately not made in the same change as the
     * three arithmetic fixes above: oPts is a point size and oFog a fog
     * coordinate, so it is a visible change to a path that cannot be run on
     * this host, and it wants its own before/after on Windows. */
    sb_append(&sb,
        "    /* Output registers (initialized to zero) */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,0);\n"
        "    float4 oPts = float4(0,0,0,0);\n"
        "    float4 oB0  = float4(0,0,0,0);\n"
        "    float4 oB1  = float4(0,0,0,0);\n"
        "    float4 oT0  = float4(0,0,0,0);\n"
        "    float4 oT1  = float4(0,0,0,0);\n"
        "    float4 oT2  = float4(0,0,0,0);\n"
        "    float4 oT3  = float4(0,0,0,0);\n"
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
     * THIRD DIVERGENCE FROM THE MSL SIBLING, AND THIS ONE IS PROBABLY RIGHT.
     * That emitter ends with the subpixel snap from prepare_vertices() and the
     * NV2A screen-space to clip-space conversion, because on the Metal path
     * those are the entire body of the fixed `vs` in nv2a_metal.m and a
     * generated vertex function replacing the interpreter has to absorb them.
     * The D3D11 path reaches the rasteriser differently and oPos is passed
     * through here untouched. NOT VERIFIED: nobody has checked what
     * src/d3d/d3d8_vsh.c's consumer expects to be in SV_POSITION, and the
     * emitted shader has never been bound to a draw. If it turns out to want
     * clip space, this is where that conversion goes. */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    o.oPos = oPos;\n"
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

