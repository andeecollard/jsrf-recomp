/* Synthetic BC1 blocks and independent expected pixels; no game assets. */
#include "nv2a_texture_copy.h"
#include "texture_copy_state.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t m[2048];
static float v[3][16][4];
static uint8_t texture[32],target[64],depth[64];
static uint32_t word(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void coords(float u,float t) {
    for(int i=0;i<3;++i) { v[i][9][0]=u; v[i][9][1]=t; }
}
int main(void) {
    NV2ATextureCopy s;
    copy_methods(m,4,4,8,16,4); modulate_methods(m);
    m[0x1b04/4]=0x02210c29; m[0x1b08/4]=0x10101;
    m[0x1b14/4]=0x01012000;
    CHECK(!nv2a_texture_copy_prepare(m,&s) && s.dxt1 && s.repeat);
    CHECK(s.width==4 && s.height==4 && nv2a_texture_copy_texture_bytes(&s)==8);
    /* Opaque red/blue endpoints; selectors 0,1,2,3 on every row. */
    le32(texture,0x001ff800); le32(texture+4,0xe4e4e4e4);
    for(int i=0;i<3;++i) {
        v[i][0][3]=v[i][9][3]=1;
        for(int k=0;k<4;++k) v[i][3][k]=1;
    }
    v[1][0][0]=8; v[2][0][1]=8;
#define DRAW() nv2a_texture_copy_triangle_depth(&s,texture,sizeof(texture),target,sizeof(target),depth,sizeof(depth),v[0],v[1],v[2])
    const uint32_t expected[]={0xffff0000,0xff0000ff,0xffaa0055,0xff5500aa};
    for(int k=0;k<4;++k) {
        coords((k+.5f)/4,.125f); CHECK(DRAW()); CHECK(word(target)==expected[k]);
    }
    coords(-.125f,1.125f); CHECK(DRAW()); CHECK(word(target)==expected[3]);
    coords(1.125f,-.875f); CHECK(DRAW()); CHECK(word(target)==expected[0]);
    s.linear=1; coords(0,.125f); CHECK(DRAW()); CHECK(word(target)==0xffaa0055); /* repeat seam */
    s.repeat=0; CHECK(DRAW()); CHECK(word(target)==expected[0]); /* clamp seam */
    /* Reversed endpoints activate BC1 transparent mode. */
    le32(texture,0xf800001f); s.linear=0;
    coords(.625f,.125f); CHECK(DRAW()); CHECK(word(target)==0xff800080);
    coords(.875f,.125f); CHECK(DRAW()); CHECK(word(target)==0);
    s.alpha_test=1; s.alpha_ref=0;
    memset(target,0x5a,sizeof(target)); CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a);
    /* Filtering produces alpha 1/2 at the opaque-red/transparent boundary.
     * The combiner receives (1/2,0,0,1/2), and source-alpha blending over
     * opaque green produces (1/4,1/2,0,3/4), including alpha channel blend. */
    s.linear=1;
    le32(texture+4,0xdddddddd); /* x0=1 red, x1=3 transparent, repeated */
    coords(.25f,.125f); s.blend=1;
    for(int i=0;i<16;++i) le32(target+4*i,0xff00ff00);
    CHECK(DRAW()); CHECK(word(target)==0xbf408000);
    s.alpha_ref=128; memset(target,0x5a,sizeof(target));
    CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a); /* GREATER, not GEQUAL */
    /* BC2/DXT3 has four-bit explicit alpha followed by a four-colour BC1
     * block. It must not use BC1's transparent three-colour endpoint rule. */
    memset(m,0,sizeof(m)); copy_methods(m,4,4,16,16,4); modulate_methods(m);
    m[0x1b04/4]=0x02210e29; m[0x1b08/4]=0x10101; m[0x1b14/4]=0x01012000;
    CHECK(!nv2a_texture_copy_prepare(m,&s) && s.dxt3 && !s.dxt1);
    CHECK(s.pitch==16 && nv2a_texture_copy_texture_bytes(&s)==16);
    memset(texture,0,sizeof(texture)); texture[0]=0xf8; /* alpha: x0=8/15, x1=15/15 */
    le32(texture+8,0xf800001f); le32(texture+12,0x0000000c); /* reversed endpoints, selectors 0 then 3 */
    coords(.125f,.125f); memset(target,0,sizeof(target)); CHECK(DRAW());
    CHECK(word(target)==0x880000ff);
    coords(.375f,.125f); memset(target,0,sizeof(target)); CHECK(DRAW());
    CHECK(word(target)==0xffaa0055);
    /* Z24 test, write mask and alpha-discard ordering. */
    s.dxt3=0; s.dxt1=1; s.pitch=8; le32(texture,0xf800001f); le32(texture+4,0xdddddddd);
    s.alpha_test=1; s.alpha_ref=0; s.blend=0; s.linear=0;
    s.depth_test=1; s.depth_write=1; s.depth_pitch=16;
    coords(.125f,.125f);
    for(int i=0;i<3;++i) v[i][0][2]=100;
    for(int i=0;i<16;++i) le32(depth+4*i,(101u<<8)|0x7b);
    CHECK(DRAW()); CHECK(word(target)==0xffff0000 && word(depth)==((100u<<8)|0x7b));
    memset(target,0x5a,sizeof(target)); CHECK(DRAW()); CHECK(word(target)==0xffff0000); /* LEQUAL */
    for(int i=0;i<3;++i) v[i][0][2]=101;
    memset(target,0x5a,sizeof(target)); CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a);
    s.depth_write=0; for(int i=0;i<3;++i) v[i][0][2]=99;
    CHECK(DRAW()); CHECK(word(target)==0xffff0000 && word(depth)==((100u<<8)|0x7b));
    s.depth_write=1; coords(.375f,.125f); memset(target,0x5a,sizeof(target));
    CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a && word(depth)==((100u<<8)|0x7b));
    /* Both winding selections and front/back culling. */
    coords(.125f,.125f); s.depth_test=0; s.cull_face=0x405; s.front_cw=1;
    CHECK(DRAW()); CHECK(word(target)==0xffff0000);
    s.front_cw=0; memset(target,0x5a,sizeof(target)); CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a);
    s.cull_face=0x404; CHECK(DRAW()); CHECK(word(target)==0xffff0000);
    s.cull_face=0x408; memset(target,0x5a,sizeof(target)); CHECK(DRAW()); CHECK(word(target)==0x5a5a5a5a);
    /* Buffer validation is before writes, even on a culled triangle. */
    CHECK(!nv2a_texture_copy_triangle_depth(&s,texture,7,target,64,depth,64,v[0],v[1],v[2]));
    s.depth_test=1;
    CHECK(!nv2a_texture_copy_triangle_depth(&s,texture,32,target,64,depth,63,v[0],v[1],v[2]));
    /* Decode the full measured loading-screen state. */
    m[0x1b04/4]=0x09910c29; m[0x300/4]=1; m[0x33c/4]=0x204;
    m[0x304/4]=1; m[0x344/4]=0x302; m[0x348/4]=0x303; m[0x350/4]=0x8006;
    m[0x308/4]=1; m[0x39c/4]=0x405; m[0x3a0/4]=0x900;
    m[0x30c/4]=1; m[0x354/4]=0x203; m[0x35c/4]=1;
    m[0x208/4]=0x123; m[0x20c/4]=16u<<16|8; m[0x310/4]=1;
    CHECK(!nv2a_texture_copy_prepare(m,&s));
    CHECK(s.width==512 && s.height==512 && nv2a_texture_copy_texture_bytes(&s)==131072);
    CHECK(s.alpha_test && s.blend && s.cull_face==0x405 && s.front_cw && s.depth_test && s.depth_write);
    m[0x290/4]=0x1000; CHECK(nv2a_texture_copy_prepare(m,&s)); /* float depth */
    m[0x290/4]=0x10000; CHECK(nv2a_texture_copy_prepare(m,&s)); /* W buffer */
    m[0x290/4]=0; m[0x1b04/4]=0x09920c29; CHECK(!nv2a_texture_copy_prepare(m,&s)); /* mip chain */
    CHECK(s.levels==2 && nv2a_texture_copy_texture_bytes(&s)==163840);
    puts("BC1 colours, alpha, wrap/filter, blending, Z24, culling and bounds passed");
}
