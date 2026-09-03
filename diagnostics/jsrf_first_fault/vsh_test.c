#include "nv2a_vsh.h"
#include "vsh_capture.h"
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
int main(void) { capture(); fields_and_parallel(); puts("NV2A captured program, field boundaries, masks and parallel execution passed"); }
