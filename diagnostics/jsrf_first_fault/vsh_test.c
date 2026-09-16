#include "nv2a_vsh.h"
#include "vsh_capture.h"
#include "vsh_encode.h"
#include "vsh_xbe_corpus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int near(float a, float b) { return fabsf(a-b) < 0.0001f; }
static void capture(void)
{
    NV2AVshProgram p;
    CHECK(nv2a_vsh_parse(jsrf_vsh_words, 12, &p));
    CHECK(p.length == 12 && p.has_final);
    CHECK(p.insns[0].mac_op == NV2A_VSH_MAC_MOV);
    CHECK(p.insns[0].mac_src[0].reg_type == NV2A_VSH_REG_INPUT);
    CHECK(p.insns[0].mac_src[0].reg_index == 0);
    CHECK(p.insns[0].mac_dst.temp_reg == 1 && p.insns[0].mac_dst.write_mask == 15);
    CHECK(p.insns[0].mac_dst.output_mask == 0);
    CHECK(p.insns[1].ilu_op == NV2A_VSH_ILU_RCP);
    CHECK(p.insns[1].ilu_src.reg_type == NV2A_VSH_REG_TEMP && p.insns[1].ilu_src.reg_index == 1);
    CHECK(p.insns[1].ilu_src.swizzle.x == 3);
    CHECK(p.insns[1].mac_dst.output_reg == NV2A_VSH_OUT_D0 && p.insns[1].mac_dst.output_mask == 15);
    CHECK(p.insns[1].ilu_dst.temp_reg == 1 && p.insns[1].ilu_dst.write_mask == 1);
    CHECK(p.insns[2].ilu_dst.output_reg == NV2A_VSH_OUT_FOG);
    CHECK(p.insns[3].mac_op == NV2A_VSH_MAC_MUL && p.insns[3].ilu_op == NV2A_VSH_ILU_MOV);
    CHECK(p.insns[3].mac_dst.temp_reg == 2 && p.insns[3].ilu_dst.output_reg == NV2A_VSH_OUT_D1);
    CHECK(p.insns[4].mac_op == NV2A_VSH_MAC_ADD);
    CHECK(p.insns[4].mac_src[2].reg_type == NV2A_VSH_REG_CONST && p.insns[4].mac_src[2].reg_index == 1);
    CHECK(p.insns[4].mac_dst.output_reg == NV2A_VSH_OUT_POS);
    CHECK(p.inputs_read == 0x1F9B); /* v0,1,3,4,7,8,9,10,11,12 */
    float in[16][4] = {{0}}, c[192][4] = {{0}};
    for (int i=0;i<16;++i) in[i][3]=1;
    c[0][0]=c[0][1]=c[0][3]=1; c[0][2]=16777215;
    c[1][0]=c[1][1]=0.53125f;
    in[3][1]=1;
    for (int i=0;i<3;++i) {
        in[0][0] = i==1 ? 2559.46875f : -0.53125f;
        in[0][1] = i==2 ? 1919.46875f : -0.53125f;
        in[9][0] = i==1 ? 2560 : 0; in[9][1] = i==2 ? 1920 : 0;
        NV2AVshResult out;
        CHECK(nv2a_vsh_execute(&p,in,c,&out));
        CHECK(near(out.output[0][0], i==1?2560:0));
        CHECK(near(out.output[0][1], i==2?1920:0));
        CHECK(near(out.output[0][3],1));
        CHECK(near(out.output[3][1],1));
        CHECK(near(out.output[9][0],in[9][0]) && near(out.output[9][1],in[9][1]));
    }
    char hlsl[32768];
    CHECK(d3d8_vsh_generate_hlsl(&p,hlsl,sizeof(hlsl)) > 0);
    CHECK(strstr(hlsl,"oD0 = (mac_result)") != NULL);
    CHECK(strstr(hlsl,"R1.w = (ilu_result).w") != NULL);
    CHECK(nv2a_vsh_parse(jsrf_vsh_words,5,&p) && !p.has_final);
    NV2AVshResult out;
    CHECK(!nv2a_vsh_execute(&p,in,c,&out));
}
static void fields_and_parallel(void)
{
    /* MOV R2,v0; MOV R2.xy,v1 + MOV R1.zw,R2 with oPos.xyzw from ILU.
     * This exercises independent masks and simultaneous operand reads. */
    const uint32_t words[] = {
        0, 0x0020001B, 0x08000000, 0x0F200000,
        0, 0x0220021B, 0x0800006C, 0x9C23F805
    };
    NV2AVshProgram p; NV2AVshResult out;
    CHECK(nv2a_vsh_parse(words,2,&p));
    CHECK(p.insns[1].ilu_src.reg_index == 2);
    CHECK(p.insns[1].mac_dst.write_mask == 12 && p.insns[1].ilu_dst.write_mask == 3);
    CHECK(p.insns[1].ilu_dst.output_mask == 15 && p.insns[1].ilu_dst.temp_reg == 1);
    float in[16][4]={{1,2,3,4},{10,20,30,40}}, c[192][4]={{0}};
    CHECK(nv2a_vsh_execute(&p,in,c,&out));
    for(int k=0;k<4;++k) CHECK(out.output[0][k] == in[0][k]);
    char hlsl[32768];
    CHECK(d3d8_vsh_generate_hlsl(&p,hlsl,sizeof(hlsl)) > 0);
    CHECK(strstr(hlsl,"float4 ilu_result = R2") < strstr(hlsl,"R2.xy = (mac_result).xy"));
    uint32_t bad[4] = {0, 0x01E0001B, 0x08000000, 1};
    CHECK(!nv2a_vsh_parse(bad,1,&p)); /* MAC 15 */
    bad[1]=0x0020001B; bad[2]=0; CHECK(!nv2a_vsh_parse(bad,1,&p)); /* used mux 0 */
    /* MOV oPos,c191.wzyx; upper constants must not clamp to zero. */
    const uint32_t high[] = {0,0x0037E0E4,0x0C000000,0x0000F801};
    CHECK(nv2a_vsh_parse(high,1,&p));
    c[191][0]=1; c[191][1]=2; c[191][2]=3; c[191][3]=4;
    CHECK(nv2a_vsh_execute(&p,in,c,&out));
    for(int k=0;k<4;++k) CHECK(out.output[0][k] == 4-k);
    /* R12/oPos alias and source C split across word 2's low bits / word 3's high bits. */
    const uint32_t alias[] = {0,0x0020001B,0x08000000,0x0FC00000,
                              0,0x02000000,0x0000006F,0x1000F84D};
    CHECK(nv2a_vsh_parse(alias,2,&p));
    CHECK(p.insns[1].ilu_src.reg_index==12);
    CHECK(nv2a_vsh_execute(&p,in,c,&out));
    for(int k=0;k<4;++k) CHECK(out.output[9][k] == in[0][k]);
}
/* ================================================================
 * Does the emitted HLSL have the shape the three arithmetic fixes require?
 *
 * WHY THIS IS A TEXT TEST AND NOT A DIFFERENTIAL ONE. The MSL sibling is
 * gated by vsh_msl_diff_test.m, which compiles the emitted shader, runs it on
 * a device and diffs all sixteen output registers against nv2a_vsh_execute.
 * There is no equivalent for HLSL on this host and there cannot be: fxc is
 * d3dcompiler_47.dll, and running a Windows x86-64 binary here needs Rosetta
 * 2, which is not installed (CrossOver's wineloader and Whisky's wine64 are
 * themselves x86-64 Mach-O and refuse to start). So this asserts the TEXT,
 * and it is worth being explicit about the difference: it can tell you the
 * emitter asked for `precise`; it cannot tell you fxc honoured it, and it
 * cannot tell you the emitted program computes what the interpreter computes.
 * The Windows measurement that would is named in nv2a_vsh_hlsl.c.
 *
 * WHAT MAKES IT MORE THAN A STRING SEARCH. Every assertion is a COUNT taken
 * from the parsed program, not a substring that happens to be somewhere in
 * 30 KB of generated code. Reverting any one of the three fixes changes a
 * count: the vsh_mul count drops to zero, the bare-multiply count rises to
 * one per MUL and MAD, the precise count drops. Applying a fix to MUL and
 * forgetting MAD fails too, which a grep for "vsh_mul" would not.
 * ================================================================ */
static int occurrences(const char *hay, const char *needle)
{
    int n = 0;
    size_t len = strlen(needle);
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += len) ++n;
    return n;
}

/* The generated body only -- the preamble contains vsh_mul's own `a * b` and
 * would make the bare-multiply count meaningless. */
static void hlsl_shape_of(const NV2AVshProgram *p, const char *what)
{
    static char hlsl[262144];
    int mul = 0, mad = 0, dst = 0, mac = 0, ilu = 0, i;
    const char *body;
    int n = d3d8_vsh_generate_hlsl(p, hlsl, sizeof(hlsl));
    if (n <= 0) { fprintf(stderr, "%s: emitter refused\n", what); exit(1); }
    body = strstr(hlsl, "--- Program body");
    CHECK(body != NULL);

    for (i = 0; i < p->length; ++i) {
        const NV2AVshInstruction *s = &p->insns[i];
        if (s->mac_op == NV2A_VSH_MAC_MUL) ++mul;
        if (s->mac_op == NV2A_VSH_MAC_MAD) ++mad;
        if (s->mac_op == NV2A_VSH_MAC_DST) ++dst;
        if (s->mac_op != NV2A_VSH_MAC_NOP && s->mac_op != NV2A_VSH_MAC_ARL) ++mac;
        if (s->ilu_op != NV2A_VSH_ILU_NOP) ++ilu;
    }

    /* 1. The a*0 rule: every MUL and every MAD product goes through the
     *    helper, and nothing else does. */
    if (occurrences(body, "vsh_mul(") != mul + mad) {
        fprintf(stderr, "%s: %d vsh_mul for %d MUL + %d MAD\n",
                what, occurrences(body, "vsh_mul("), mul, mad);
        exit(1);
    }
    /* 2. ...so the only bare multiply left in a body is DST's, which
     *    deliberately keeps one because mac_eval does not route DST through
     *    multiply() either. */
    if (occurrences(body, " * ") != dst) {
        fprintf(stderr, "%s: %d bare multiplies for %d DST\n",
                what, occurrences(body, " * "), dst);
        exit(1);
    }
    /* 3. Contraction: every instruction result carries `precise`. Counting
     *    both forms is what makes this catch a half-applied fix -- the
     *    unqualified count must not exceed the qualified one. */
    if (occurrences(body, "precise float4 mac_result") != mac
        || occurrences(body, "float4 mac_result") != mac
        || occurrences(body, "precise float4 ilu_result") != ilu
        || occurrences(body, "float4 ilu_result") != ilu) {
        fprintf(stderr, "%s: precise %d/%d mac, %d/%d ilu (want %d, %d)\n",
                what, occurrences(body, "precise float4 mac_result"),
                occurrences(body, "float4 mac_result"),
                occurrences(body, "precise float4 ilu_result"),
                occurrences(body, "float4 ilu_result"), mac, ilu);
        exit(1);
    }
}

static void hlsl_arithmetic_shape(void)
{
    static char hlsl[262144];
    char path[1024];
    NV2AVshProgram p;
    uint32_t w[4];
    VshIns ins;

    /* --- LIT, which no program in the title contains, so it is a fixture and
     * says so. The two LACKS below are absence-measurements; each is paired
     * with a needle that IS in the same buffer, so a strstr that found nothing
     * because the buffer was empty would fail the positive control first. */
    memset(&ins, 0, sizeof(ins));
    ins.ilu = NV2A_VSH_ILU_LIT; ins.mac = NV2A_VSH_MAC_DST;
    ins.input_index = 0;
    ins.a.mux = 2; ins.a.swz = SWZ_ID;
    ins.b.mux = 2; ins.b.swz = SWZ_ID;
    ins.c.mux = 2; ins.c.swz = SWZ_ID;
    ins.mac_temp = 1; ins.mac_mask = 15; ins.ilu_mask = 15;
    ins.out_mask = 15; ins.out_reg = NV2A_VSH_OUT_POS; ins.final = 1;
    vsh_encode(w, &ins);
    CHECK(nv2a_vsh_parse(w, 1, &p));
    CHECK(p.insns[0].ilu_op == NV2A_VSH_ILU_LIT);   /* encoder, not decoder */
    CHECK(p.insns[0].mac_op == NV2A_VSH_MAC_DST);
    CHECK(d3d8_vsh_generate_hlsl(&p, hlsl, sizeof(hlsl)) > 0);
    CHECK(strstr(hlsl, "pow(max(") != NULL);        /* positive controls */
    CHECK(strstr(hlsl, "127.99609375") != NULL);
    CHECK(strstr(hlsl, "clamp(") != NULL);
    CHECK(strstr(hlsl, "1e-30") == NULL);           /* the epsilon is gone */
    CHECK(strstr(hlsl, "-128.0, 128.0") == NULL);   /* and so is the bound */
    /* 0^0: fxc lowers pow to exp2(y*log2(x)), which gives NaN there where
     * powf gives 1, so the exponent is guarded. */
    CHECK(strstr(hlsl, ".w == 0.0) ? 1.0 : pow(") != NULL);
    hlsl_shape_of(&p, "LIT+DST fixture");
    /* DST is the positive control for the bare-multiply count above: this
     * program HAS one, so "count == dst" is not passing because " * " can
     * never be found. */
    CHECK(occurrences(strstr(hlsl, "--- Program body"), " * ") == 1);

    /* --- MAD, which neither the captured program nor the fixture above
     * contains. Without it, reverting the MAD half of the zero-rule fix is
     * caught only when the title's XBE is reachable, and the corpus is
     * optional -- so the gate would depend on an environment variable. */
    memset(&ins, 0, sizeof(ins));
    ins.mac = NV2A_VSH_MAC_MAD;
    ins.input_index = 0; ins.const_index = 3;
    ins.a.mux = 2; ins.a.swz = SWZ_ID;   /* v0 */
    ins.b.mux = 3; ins.b.swz = SWZ_ID;   /* c[3] */
    ins.c.mux = 1; ins.c.temp = 4; ins.c.swz = SWZ_ID;  /* R4 */
    ins.mac_temp = 5; ins.mac_mask = 15;
    ins.out_mask = 15; ins.out_reg = NV2A_VSH_OUT_POS; ins.final = 1;
    vsh_encode(w, &ins);
    CHECK(nv2a_vsh_parse(w, 1, &p));
    CHECK(p.insns[0].mac_op == NV2A_VSH_MAC_MAD);   /* encoder, not decoder */
    hlsl_shape_of(&p, "MAD fixture");
    CHECK(d3d8_vsh_generate_hlsl(&p, hlsl, sizeof(hlsl)) > 0);
    CHECK(strstr(hlsl, "mac_result = (vsh_mul(v0, c[3]) + R4);") != NULL);

    /* --- MUL and MAD on the captured program, which the title really ran. */
    CHECK(nv2a_vsh_parse(jsrf_vsh_words, 12, &p));
    hlsl_shape_of(&p, "captured program");
    CHECK(d3d8_vsh_generate_hlsl(&p, hlsl, sizeof(hlsl)) > 0);
    CHECK(strstr(hlsl, "float4 vsh_mul(float4 a, float4 b)") != NULL);
    CHECK(strstr(hlsl, "mac_result = vsh_mul(R1, c[0]);") != NULL);

    /* --- The title's own programs, when the dump is reachable. This is the
     * part that covers MAD: the captured program contains none, and a corpus
     * that exercised neither opcode would let both fixes be reverted without
     * a failure -- so the opcode totals are checked for being nonzero, and
     * printed either way. */
    if (vsh_xbe_default_path(path, sizeof(path))) {
        static VshXbeProgram progs[VSH_XBE_MAX_PROGRAMS];
        VshXbeScan info;
        int count = vsh_xbe_scan(path, progs, VSH_XBE_MAX_PROGRAMS, &info);
        if (count > 0) {
            int i, mul = 0, mad = 0, dst = 0, lit = 0;
            for (i = 0; i < count; ++i) {
                int k;
                CHECK(nv2a_vsh_parse(progs[i].words, progs[i].length, &p));
                hlsl_shape_of(&p, path);
                for (k = 0; k < p.length; ++k) {
                    if (p.insns[k].mac_op == NV2A_VSH_MAC_MUL) ++mul;
                    if (p.insns[k].mac_op == NV2A_VSH_MAC_MAD) ++mad;
                    if (p.insns[k].mac_op == NV2A_VSH_MAC_DST) ++dst;
                    if (p.insns[k].ilu_op == NV2A_VSH_ILU_LIT) ++lit;
                }
            }
            printf("[VSH-HLSL] %d title programs: %d MUL, %d MAD, %d DST,"
                   " %d LIT -- shape checked\n", count, mul, mad, dst, lit);
            CHECK(mul > 0 && mad > 0);  /* or the loop proved nothing */
        } else {
            printf("[VSH-HLSL] no programs read from %s -- fixtures only\n",
                   path);
        }
    } else {
        printf("[VSH-HLSL] NO TITLE CORPUS: set JSRF_GAME_DIR or"
               " JSRF_VSH_CORPUS_XBE -- fixtures only\n");
    }
}

int main(void) { capture(); fields_and_parallel(); hlsl_arithmetic_shape(); puts("NV2A captured program, field boundaries, masks, parallel execution and HLSL arithmetic shape passed"); }
