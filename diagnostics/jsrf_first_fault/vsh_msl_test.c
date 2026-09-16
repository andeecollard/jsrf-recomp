/* What the MSL emitter emits, for programs whose encoding this file builds
 * field by field rather than pasting hex.
 *
 * WHY AN ENCODER AND NOT CAPTURED WORDS. vsh_test.c already covers the real
 * captured JSRF program, and this file uses it too -- but a captured program
 * happens to contain the opcodes the title happens to use. It has no DST, no
 * SLT, no ARL, no a0-relative constant read. Those are exactly the cases an
 * emitter gets wrong, so they have to be constructed. vsh_encode.h mirrors
 * the field layout in nv2a_vsh_parse; if the decoder's bit positions ever
 * move, these programs stop decoding as intended and the CHECKs on the parse
 * results -- which come first, deliberately -- fail before any text is
 * compared. A text expectation resting on a silently misdecoded program would
 * pass while proving nothing.
 *
 * WHAT THIS DOES NOT PROVE. Only that the generator produces the expected
 * text. It says nothing about whether that text renders the same image as
 * nv2a_vsh_execute; no MSL here has ever been bound to a pipeline. Syntactic
 * validity is a separate, device-dependent test: vsh_msl_compile_test.m.
 */
#include "nv2a_vsh.h"
#include "vsh_capture.h"
#include "vsh_encode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
/* Report the emitted text once, on failure: a bare "strstr != NULL" tells you
 * nothing about what was emitted instead. */
#define HAS(s, needle) do { if (!strstr((s), (needle))) { \
    fprintf(stderr, "%s:%d: expected %s in:\n%s\n", __FILE__, __LINE__, #needle, (s)); exit(1); } } while (0)
#define LACKS(s, needle) do { if (strstr((s), (needle))) { \
    fprintf(stderr, "%s:%d: unexpected %s in:\n%s\n", __FILE__, __LINE__, #needle, (s)); exit(1); } } while (0)

static const char *emit(const VshIns *program, int count, char *buf, int bufsize)
{
    uint32_t words[8 * 4];
    NV2AVshProgram p;
    CHECK(count > 0 && count <= 8);
    for (int i = 0; i < count; ++i) vsh_encode(words + i * 4, &program[i]);
    CHECK(nv2a_vsh_parse(words, count, &p));
    CHECK(p.length == count && p.has_final);
    CHECK(nv2a_vsh_generate_msl(&p, buf, bufsize) > 0);
    return buf;
}

/* The scaffolding the emitter always produces, independent of the program. */
static void preamble(void)
{
    char msl[32768];
    /* MOV oPos, v0; the simplest complete program there is. */
    VshIns prog[] = {{ .mac = NV2A_VSH_MAC_MOV, .input_index = 0,
                    .a = {2, 0, SWZ_ID, 0}, .mac_temp = 0, .mac_mask = 0,
                    .out_mask = 15, .out_reg = NV2A_VSH_OUT_POS, .final = 1 }};
    emit(prog, 1, msl, sizeof(msl));

    HAS(msl, "#include <metal_stdlib>");
    HAS(msl, "using namespace metal;");
    /* Inputs: generic attribute indices, only the ones the program reads. */
    HAS(msl, "struct VS_IN {\n    float4 v0 [[attribute(0)]];\n};");
    LACKS(msl, "v1 [[attribute(1)]]");
    /* Outputs: one [[position]], everything else a plain varying. */
    HAS(msl, "float4 oPos [[position]];");
    HAS(msl, "float  oPts [[point_size]];");
    /* Bindings the design note commits to. */
    HAS(msl, "vertex VS_OUT vsh_main(VS_IN input [[stage_in]],");
    HAS(msl, "constant float4 *c [[buffer(1)]],");
    HAS(msl, "constant VSH_Viewport &viewport [[buffer(2)]]");
    /* Register file. */
    HAS(msl, "float4 R0 = float4(0,0,0,0);");
    HAS(msl, "float4 R11 = float4(0,0,0,0);");
    LACKS(msl, "float4 R12 = ");          /* R12 is oPos, never its own variable */
    HAS(msl, "#define R12 oPos");
    HAS(msl, "#undef R12");
    HAS(msl, "int a0 = 0;");
    HAS(msl, "float4 v0 = input.v0;");
    /* nv2a_vsh_execute seeds every output's w to 1 before the program runs.
     * The HLSL emitter zeroes oT0-oT3, oFog, oPts, oB0 and oB1 instead; this
     * one does not, deliberately. */
    HAS(msl, "float4 oT0  = float4(0,0,0,1);");
    HAS(msl, "float4 oFog = float4(0,0,0,1);");
    /* The screen-space fixup this emitter absorbs from the CPU path. */
    HAS(msl, "trunc(snapped.x * 16.0f) / 16.0f");
    HAS(msl, "o.oPos = float4((snapped.x / viewport.width * 2.0f - 1.0f) * oPos.w,");
    HAS(msl, "o.oD0  = saturate(oD0);");
    HAS(msl, "o.oFog = oFog.x;");

    /* MSL has no scalar swizzle, so the HLSL splat idiom -- dot(a,b).xxxx,
     * (1.0/x).xxxx -- will not compile. `.xxxx` on a source REGISTER is a
     * perfectly ordinary vector swizzle and does occur; it is a swizzle on a
     * parenthesised expression result that must never be emitted. Negation
     * wraps outside the swizzle, (-R0.xxxx), so this cannot false-positive. */
    LACKS(msl, ").xxxx");

    /* A program reading no attributes gets no stage_in argument: an empty
     * [[stage_in]] struct is not legal MSL. MOV oPos, c[0]. */
    VshIns noinput[] = {{ .mac = NV2A_VSH_MAC_MOV, .const_index = 0,
                       .a = {3, 0, SWZ_ID, 0}, .out_mask = 15,
                       .out_reg = NV2A_VSH_OUT_POS, .final = 1 }};
    emit(noinput, 1, msl, sizeof(msl));
    LACKS(msl, "struct VS_IN");
    LACKS(msl, "stage_in");
    HAS(msl, "vertex VS_OUT vsh_main(constant float4 *c [[buffer(1)]],");
    HAS(msl, "float4 mac_result = c[0];");
}

/* One instruction per opcode shape the task named, plus the operand
 * decorations: swizzle, write mask, negation, constant read. */
static void operands_and_opcodes(void)
{
    char msl[32768];

    /* MOV R3, v8 -- plain move, identity swizzle, full mask, no output. */
    {
        VshIns prog[] = {{ .mac = NV2A_VSH_MAC_MOV, .input_index = 8,
                        .a = {2, 0, SWZ_ID, 0}, .mac_temp = 3, .mac_mask = 15,
                        .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 mac_result = v8;\n");
        HAS(msl, "    R3 = (mac_result);\n");
        LACKS(msl, "v8.");              /* identity swizzle is not spelled out */
    }

    /* DP4 oPos, v0, c[96] -- constant-file read, dot splat, output write. */
    {
        VshIns prog[] = {{ .mac = NV2A_VSH_MAC_DP4, .input_index = 0,
                        .const_index = 96,
                        .a = {2, 0, SWZ_ID, 0}, .b = {3, 0, SWZ_ID, 0},
                        .mac_temp = 0, .mac_mask = 0,
                        .out_mask = 15, .out_reg = NV2A_VSH_OUT_POS,
                        .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 mac_result = float4(dot(v0, c[96]));\n");
        HAS(msl, "    oPos = (mac_result);\n");
    }

    /* DP4 oPos.y, v0, c[a0 + 5] -- a0-relative constant read and a
     * single-component output mask. The 8-bit wrap is part of the semantics:
     * the hardware wraps a relative index before any bounds check, so
     * read_source in nv2a_vsh.c masks too. The clamp after it is not: it is
     * this emitter's substitute for the interpreter's "refuse the draw", which
     * a vertex function cannot do. 192 float4 is also 3072 bytes, inside
     * Metal's 4 KB setVertexBytes limit; 256 would not be. */
    {
        VshIns prog[] = {{ .mac = NV2A_VSH_MAC_DP4, .input_index = 0,
                        .const_index = 5,
                        .a = {2, 0, SWZ_ID, 0}, .b = {3, 0, SWZ_ID, 0},
                        .out_mask = 4, .out_reg = NV2A_VSH_OUT_POS,
                        .rel = 1, .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 mac_result = float4(dot(v0, c[min((a0 + 5) & 255, 191)]));\n");
        HAS(msl, "    oPos.y = (mac_result).y;\n");
    }

    /* MAD R2.xz, -v1.yyzw, c[3], R1 -- negation, a non-identity swizzle, a
     * gapped write mask, and a three-source op, in one instruction. */
    {
        VshIns prog[] = {{ .mac = NV2A_VSH_MAC_MAD, .input_index = 1,
                        .const_index = 3,
                        .a = {2, 0, SWZ(1,1,2,3), 1},
                        .b = {3, 0, SWZ_ID, 0},
                        .c = {1, 1, SWZ_ID, 0},
                        .mac_temp = 2, .mac_mask = 10 /* .xz */,
                        .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 mac_result = (vsh_mul((-v1.yyzw), c[3]) + R1);\n");
        HAS(msl, "    R2.xz = (mac_result).xz;\n");
    }

    /* The remaining MAC shapes, one instruction each. */
    {
        struct { unsigned op; const char *expect; } cases[] = {
            { NV2A_VSH_MAC_MUL, "float4 mac_result = vsh_mul(v0, c[7]);\n" },
            { NV2A_VSH_MAC_DP3, "float4 mac_result = float4(dot(v0.xyz, c[7].xyz));\n" },
            { NV2A_VSH_MAC_DPH, "float4 mac_result = float4(dot(float4(v0.xyz, 1.0f), c[7]));\n" },
            { NV2A_VSH_MAC_DST, "float4 mac_result = float4(1.0f, v0.y * c[7].y, v0.z, c[7].w);\n" },
            { NV2A_VSH_MAC_MIN, "float4 mac_result = min(v0, c[7]);\n" },
            { NV2A_VSH_MAC_MAX, "float4 mac_result = max(v0, c[7]);\n" },
            { NV2A_VSH_MAC_SLT, "float4 mac_result = (1.0f - step(c[7], v0));\n" },
            { NV2A_VSH_MAC_SGE, "float4 mac_result = step(c[7], v0);\n" },
        };
        for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); ++k) {
            VshIns prog[] = {{ .mac = cases[k].op, .input_index = 0, .const_index = 7,
                            .a = {2, 0, SWZ_ID, 0}, .b = {3, 0, SWZ_ID, 0},
                            .mac_temp = 4, .mac_mask = 15, .final = 1 }};
            emit(prog, 1, msl, sizeof(msl));
            HAS(msl, cases[k].expect);
        }
    }

    /* ADD reads A and C, not A and B -- the one MAC op whose second operand is
     * source C. Getting this wrong is invisible in a program where B and C
     * happen to be the same register, so use different ones. */
    {
        VshIns prog[] = {{ .mac = NV2A_VSH_MAC_ADD, .input_index = 2, .const_index = 9,
                        .a = {2, 0, SWZ_ID, 0}, .b = {1, 5, SWZ_ID, 0}, .c = {3, 0, SWZ_ID, 0},
                        .mac_temp = 6, .mac_mask = 15, .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 mac_result = (v2 + c[9]);\n");
        /* R5 is source B and must not appear in the expression. It still
         * appears as a temp-register declaration, so match the operator. */
        LACKS(msl, "+ R5");
        LACKS(msl, "R5 +");
    }

    /* ARL: writes the address register, not a destination. The write happens
     * after both units have produced their values, so the constant read in the
     * next instruction is the one that sees it. */
    {
        VshIns prog[] = {
            { .mac = NV2A_VSH_MAC_ARL, .input_index = 3,
              .a = {2, 0, SWZ(1,1,1,1), 0}, .mac_temp = 0, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_MOV, .const_index = 0, .a = {3, 0, SWZ_ID, 0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_POS, .rel = 1, .final = 1 },
        };
        emit(prog, 2, msl, sizeof(msl));
        HAS(msl, "    int next_address = int(floor(v3.y));\n");
        HAS(msl, "    a0 = next_address;\n");
        HAS(msl, "float4 mac_result = c[min((a0 + 0) & 255, 191)];\n");
        /* ARL has no destination write: the decoder clears its mask. */
        LACKS(msl, "R0 = (mac_result)");
    }
}

/* ILU shapes. The decoder replicates the shared C swizzle across all four
 * components for the scalar ILU ops, and the emitter then takes .x of it. */
static void ilu_opcodes(void)
{
    char msl[32768];
    struct { unsigned op; const char *expect; } cases[] = {
        { NV2A_VSH_ILU_MOV, "float4 ilu_result = R1.zzzz;\n" },
        { NV2A_VSH_ILU_RCP, "float4 ilu_result = float4(1.0f / R1.z);\n" },
        { NV2A_VSH_ILU_RCC, "float4 ilu_result = vsh_rcc(R1.z);\n" },
        { NV2A_VSH_ILU_RSQ, "float4 ilu_result = float4(rsqrt(abs(R1.z)));\n" },
        { NV2A_VSH_ILU_EXP, "float4 ilu_result = vsh_exp(R1.z);\n" },
        { NV2A_VSH_ILU_LOG, "float4 ilu_result = vsh_log(R1.z);\n" },
    };
    for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); ++k) {
        VshIns prog[] = {{ .ilu = cases[k].op,
                        .c = {1, 1, SWZ(2,2,2,2), 0},
                        .mac_temp = 5, .ilu_mask = 15, .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, cases[k].expect);
        HAS(msl, "    R5 = (ilu_result);\n");
    }
    /* The helpers the generated code calls must be defined in every shader. */
    /* The helper that carries NV2A's multiply-by-zero rule, and the pragma
     * that stops the compiler fusing the MAD it feeds. Both are emitter
     * FIXES, measured against the interpreter in vsh_msl_diff_test; a shader
     * emitted without either one computes different numbers. */
    HAS(msl, "float4 vsh_mul(float4 a, float4 b)");
    HAS(msl, "#pragma clang fp contract(off)");
    HAS(msl, "float4 vsh_rcc(float x)");
    HAS(msl, "as_type<uint>(r)");
    HAS(msl, "float4 vsh_exp(float x)");
    HAS(msl, "float4 vsh_log(float x)");

    /* LIT is expanded inline, on the unscalarised source.
     *
     * pow(), not exp2(w * log2(y + 1e-30)), and the exponent clamp is the
     * interpreter's 127.99609375 rather than 128. The old form reads as the
     * same function and is not: for any y <= 0 with a negative exponent the
     * interpreter returns +inf and it returned a large finite number, which
     * vsh_msl_diff_test caught on 59 of 64 fixture vectors. No JSRF program
     * contains a LIT, so that fixture is the only evidence either way. */
    {
        VshIns prog[] = {{ .ilu = NV2A_VSH_ILU_LIT, .c = {1, 2, SWZ_ID, 0},
                        .mac_temp = 3, .ilu_mask = 15, .final = 1 }};
        emit(prog, 1, msl, sizeof(msl));
        HAS(msl, "float4 ilu_result = float4(1.0f, max(R2.x, 0.0f), (R2.x > 0.0f) ?"
                 " pow(max(R2.y, 0.0f),"
                 " clamp(R2.w, -127.99609375f, 127.99609375f)) : 0.0f, 1.0f);\n");
    }
}

/* Both units in one slot read the pre-instruction register state, so both
 * results must be computed before either is stored. This is the classic way
 * to get a paired MAC/ILU slot subtly wrong, and text order is the only thing
 * this test can check about it. */
static void paired_units(void)
{
    char msl[32768];
    /* MUL R2, v0, c[4] issued with RCP, whose destination the decoder forces
     * to R1 when the slot is paired. */
    VshIns prog[] = {{ .mac = NV2A_VSH_MAC_MUL, .ilu = NV2A_VSH_ILU_RCP,
                    .input_index = 0, .const_index = 4,
                    .a = {2, 0, SWZ_ID, 0}, .b = {3, 0, SWZ_ID, 0},
                    .c = {1, 0, SWZ(3,3,3,3), 0},
                    .mac_temp = 2, .mac_mask = 15,
                    .ilu_mask = 1 /* .w */, .final = 1 }};
    emit(prog, 1, msl, sizeof(msl));
    const char *mac_value = strstr(msl, "float4 mac_result = vsh_mul(v0, c[4]);");
    const char *ilu_value = strstr(msl, "float4 ilu_result = float4(1.0f / R0.w);");
    const char *mac_store = strstr(msl, "R2 = (mac_result);");
    const char *ilu_store = strstr(msl, "R1.w = (ilu_result).w;");
    CHECK(mac_value && ilu_value && mac_store && ilu_store);
    CHECK(mac_value < ilu_value);
    CHECK(ilu_value < mac_store);   /* both values before either store */
    CHECK(mac_store < ilu_store);
}

/* The captured JSRF program, so this file is anchored to something the title
 * actually uploaded and not only to programs written to please it. */
static void captured_program(void)
{
    char msl[32768], hlsl[32768];
    NV2AVshProgram p;
    CHECK(nv2a_vsh_parse(jsrf_vsh_words, 12, &p));
    int n = nv2a_vsh_generate_msl(&p, msl, sizeof(msl));
    CHECK(n > 0 && n == (int)strlen(msl));
    /* The same two writes vsh_test.c pins in the HLSL. */
    HAS(msl, "oD0 = (mac_result)");
    HAS(msl, "R1.w = (ilu_result).w");
    /* v0,1,3,4,7,8,9,10,11,12 are read; v2 and v5 are not. */
    CHECK(p.inputs_read == 0x1F9B);
    HAS(msl, "float4 v12 [[attribute(12)]];");
    LACKS(msl, "float4 v2 [[attribute(2)]];");
    /* The HLSL emitter is still the reference for structure; where the two
     * differ it should be for a reason named in nv2a_vsh_msl.c. */
    CHECK(d3d8_vsh_generate_hlsl(&p, hlsl, sizeof(hlsl)) > 0);
    LACKS(msl, ").xxxx");
    LACKS(msl, "cbuffer");
    LACKS(msl, "SV_POSITION");
    /* Positive controls: the HLSL this is being contrasted with really does
     * contain both, so the two LACKS above are testing something. */
    CHECK(strstr(hlsl, "cbuffer") != NULL);
    CHECK(strstr(hlsl, "SV_POSITION") != NULL);
    /* And this program really does use a source swizzle, so the narrowing of
     * the splat check above is not hiding an absent case. */
    HAS(msl, "v1.xxxx");
    CHECK(strstr(hlsl, ").xxxx") != NULL);
}

/* Everything the emitter must refuse rather than emit something plausible. */
static void rejections(void)
{
    char msl[32768];
    uint32_t words[4 * 4];
    NV2AVshProgram p;
    VshIns mov = { .mac = NV2A_VSH_MAC_MOV, .input_index = 0, .a = {2, 0, SWZ_ID, 0},
                .mac_temp = 0, .mac_mask = 15, .final = 1 };

    /* No FINAL: a decodable prefix is not a runnable program. */
    VshIns prefix = mov; prefix.final = 0;
    vsh_encode(words, &prefix);
    CHECK(nv2a_vsh_parse(words, 1, &p) && !p.has_final);
    CHECK(nv2a_vsh_generate_msl(&p, msl, sizeof(msl)) == 0);

    /* A write to the constant file. The interpreter rejects these too
     * (nv2a_vsh_execute returns 0), so emitting for them would put the Metal
     * path ahead of the CPU path on a case neither implements. Clearing bit 11
     * of word 3 turns the output-register write into a constant write. */
    vsh_encode(words, &mov);
    words[3] &= ~(1u << 11);
    words[3] |= (15u << 12) | (8u << 3);   /* mask + constant index 8 */
    CHECK(nv2a_vsh_parse(words, 1, &p));
    CHECK(p.insns[0].mac_dst.constant_reg == 8);
    CHECK(nv2a_vsh_generate_msl(&p, msl, sizeof(msl)) == 0);

    /* An undecodable program. */
    uint32_t bad[4] = {0, 0x01E0001B, 0x08000000, 1};   /* MAC opcode 15 */
    CHECK(!nv2a_vsh_parse(bad, 1, &p));
    CHECK(nv2a_vsh_generate_msl(&p, msl, sizeof(msl)) == 0);

    /* Truncation is a failure, not a short shader. */
    vsh_encode(words, &mov);
    CHECK(nv2a_vsh_parse(words, 1, &p));
    char small[600];
    CHECK(nv2a_vsh_generate_msl(&p, small, sizeof(small)) == 0);
    CHECK(nv2a_vsh_generate_msl(&p, msl, sizeof(msl)) > 0);

    /* Null arguments. */
    CHECK(nv2a_vsh_generate_msl(NULL, msl, sizeof(msl)) == 0);
    CHECK(nv2a_vsh_generate_msl(&p, NULL, sizeof(msl)) == 0);
    CHECK(nv2a_vsh_generate_msl(&p, msl, 0) == 0);
}

int main(void)
{
    /* The text expectations below describe the emitter with the dot-product
     * zero rule OFF: plain dot() for DP3/DPH/DP4. RECOMP_VSH_DP_ZERO has
     * defaulted ON since 16 Sep 2026, so pin the arm here rather than let the
     * default decide which emitter this test is looking at. The ON emitter
     * (vsh_dp3/vsh_dp4) is verified numerically over the title's own 126
     * programs by jsrf_vsh_msl_diff_test, in both modes. */
    setenv("RECOMP_VSH_DP_ZERO", "0", 1);
    preamble();
    operands_and_opcodes();
    ilu_opcodes();
    paired_units();
    captured_program();
    rejections();
    puts("NV2A MSL emission: preamble, all 13 non-NOP MAC and 7 non-NOP ILU"
         " opcodes, swizzles, masks, negation, constant reads including"
         " a0-relative, paired slots and rejections passed");
    return 0;
}
