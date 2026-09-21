#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void put(uint32_t *m,unsigned offset,float f) { memcpy(m+offset/4,&f,4); }
int main(void)
{
    uint32_t m[2048]={0}; float in[16][4]={0},out[16][4];
    /* Asymmetric matrix catches transposition; W division precedes viewport offset. */
    const float a[16]={2,0,0,10, 0,3,0,20, 0,0,4,30, 0,0,0,2};
    for(unsigned i=0;i<16;++i) put(m,0x680+4*i,a[i]);
    put(m,0xa20,.53125f);put(m,0xa24,.53125f);
    in[0][0]=1;in[0][1]=2;in[0][2]=3;in[0][3]=1;
    in[3][0]=.25f;in[4][1]=.5f;in[10][0]=.75f;in[10][3]=1;
    CHECK(!nv2a_ff_vertex(m,in,out));
    CHECK(out[0][0]==6.53125f && out[0][1]==13.53125f && out[0][2]==21 && out[0][3]==2);
    CHECK(out[3][0]==.25f && out[4][1]==.5f && out[10][0]==.75f && out[10][3]==1);
    m[0x424/4]=1;
    for(unsigned i=0;i<4;++i) put(m,0x700+20*i,1);
    put(m,0x70c,.125f);
    CHECK(!nv2a_ff_vertex(m,in,out));CHECK(out[10][0]==.875f);
    /* Normal-map texgen uses the inverse model-view 3x3, then the texture matrix. */
    m[0x420/4]=1;in[2][0]=3;in[2][1]=4;in[9][3]=1;
    for(unsigned i=0;i<3;++i) put(m,0x580+20*i,1);
    for(unsigned i=0;i<4;++i) put(m,0x6c0+20*i,1);
    put(m,0x6cc,.2f);put(m,0x6dc,-1.1f);
    m[0x3a4/4]=1;m[0x3c0/4]=m[0x3c4/4]=m[0x3c8/4]=0x8511;
    CHECK(!nv2a_ff_vertex(m,in,out));
    fprintf(stderr,"normal-map output %.9g %.9g %.9g %.9g\n",out[9][0],out[9][1],out[9][2],out[9][3]);
    CHECK(fabsf(out[9][0]-.8f)<1e-6f && fabsf(out[9][1]+.3f)<1e-6f
            && out[9][2]==0 && out[9][3]==1);
    m[0x3cc/4]=0x8511;CHECK(nv2a_ff_vertex(m,in,out));m[0x3cc/4]=0;
    m[0x3c0/4]=0x8512;CHECK(nv2a_ff_vertex(m,in,out));m[0x3c0/4]=0;
    /* Ambient plus one infinite directional light, the path measured in the
     * first rejected JSRF city batch.
     *
     * THE NORMAL POINTS ALONG THE REGISTER, NOT AGAINST IT. The infinite
     * direction register already holds the direction TO the light (D3D
     * negates D3DLIGHT_DIRECTIONAL.Direction before writing it), and the
     * dot product is taken as given: xemu forms max(0, dot(tNormal,
     * lightDirection)). This test used to put the normal at +z against a
     * register of -z and expect full diffuse, which encoded the negated
     * form; that form drew JSRF's lit Load screen scene-ambient brown where
     * xemu draws the light's yellow (21 Sep 2026). */
    in[2][0]=in[2][1]=0;in[2][2]=-1;in[3][0]=in[3][1]=in[3][2]=.5f;in[3][3]=.75f;
    for(unsigned k=0;k<3;++k) { put(m,0xa10+4*k,.25f);put(m,0x3a8+4*k,.1f);put(m,0x1000+4*k,.2f);put(m,0x100c+4*k,.5f); }
    put(m,0x3b4,.8f);put(m,0x1034,0);put(m,0x1038,0);put(m,0x103c,-1);
    m[0x3bc/4]=1;m[0x314/4]=1;
    CHECK(!nv2a_ff_vertex(m,in,out));
    for(unsigned k=0;k<3;++k)CHECK(fabsf(out[3][k]-.575f)<1e-6f);
    CHECK(fabsf(out[3][3]-.6f)<1e-6f);
    m[0x3bc/4]=2;CHECK(nv2a_ff_vertex(m,in,out));m[0x3bc/4]=0;m[0x314/4]=0;
    m[0x328/4]=1;CHECK(nv2a_ff_vertex(m,in,out));m[0x328/4]=0;
    put(m,0x6bc,0);CHECK(nv2a_ff_vertex(m,in,out));
    puts("Fixed-function projection, colors, secondary texture matrix and rejection passed");
    return 0;
}
