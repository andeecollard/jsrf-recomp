#include "nv2a_texture_copy.h"
#include "nv2a_vsh.h"
#include "texture_copy_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t methods[2048];
static uint8_t ramin[0x100000];
static float v[3][16][4];
static void vertices(void) {
    memset(v,0,sizeof(v));
    for(int i=0;i<3;++i) {
        v[i][0][3]=v[i][9][3]=1;
        v[i][3][3]=i==1 ? .75f : .25f;
    }
    v[1][0][0]=v[1][9][0]=8;
    v[2][0][1]=v[2][9][1]=8;
}
static uint32_t pixel(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int main(void) {
    NV2ATextureCopy s;
    const uint8_t texture[16]={0,0xf8,0xe0,7,0x1f,0,0x55,0xaa,0x1f,0,0xe0,7,0,0xf8,0x55,0xaa};
    uint8_t target[40];
    uint32_t base,limit;
    copy_dma(ramin,0x2345,0xffff);
    CHECK(nv2a_dma_resolve(ramin,sizeof(ramin),0x03000000,3,&base,&limit));
    CHECK(base==0x2345 && limit==0xffff);
    CHECK(!nv2a_dma_resolve(ramin,4096,0,3,&base,&limit));
    CHECK(!nv2a_dma_resolve(ramin,sizeof(ramin),0,77,&base,&limit));
    le32(ramin+0x1120,0x2003d); /* PCI DMA unsupported */
    CHECK(!nv2a_dma_resolve(ramin,sizeof(ramin),0,3,&base,&limit));
    copy_methods(methods,3,2,8,20,4);
    CHECK(!nv2a_texture_copy_prepare(methods,&s));
    vertices(); memset(target,0xcc,sizeof(target));
#define DRAW() nv2a_texture_copy_triangle(&s,texture,sizeof(texture),target,sizeof(target),v[0],v[1],v[2])
    CHECK(DRAW());
    CHECK(pixel(target)==0x48ff0000); /* texture RGB and smoothly interpolated vertex alpha */
    CHECK((pixel(target+4)&0xffffff)==0x00ff00);
    CHECK((pixel(target+8)&0xffffff)==0x0000ff);
    CHECK((pixel(target+20)&0xffffff)==0x0000ff);
    for(int i=12;i<20;++i) CHECK(target[i]==0xcc); /* padding untouched */
    for(int i=32;i<40;++i) CHECK(target[i]==0xcc);
    for(int i=0;i<3;++i) { v[i][9][0]=2; v[i][9][1]=1; v[i][9][3]=2; }
    CHECK(DRAW()); CHECK((pixel(target)&0xffffff)==0x808000); /* projective Q, bilinear midpoint */
    s.linear=0; CHECK(DRAW()); CHECK((pixel(target)&0xffffff)==0x00ff00);
    for(int i=0;i<3;++i) { v[i][9][0]=-1e30f; v[i][9][1]=-1e30f; }
    CHECK(DRAW()); CHECK((pixel(target)&0xffffff)==0xff0000); /* clamp safely before integer conversion */
    vertices(); s.linear=1; v[1][0][3]=2;
    CHECK(DRAW()); CHECK((pixel(target)>>24)==68); /* reciprocal W, not W */
    v[1][9][3]=0; memset(target,0xcc,sizeof(target));
    CHECK(!DRAW()); for(unsigned i=0;i<sizeof(target);++i) CHECK(target[i]==0xcc);
    vertices(); CHECK(!nv2a_texture_copy_triangle(&s,texture,4,target,sizeof(target),v[0],v[1],v[2]));
    methods[0x304/4]=1; CHECK(nv2a_texture_copy_prepare(methods,&s));
    methods[0x304/4]=0; methods[0xac0/4]^=1; CHECK(nv2a_texture_copy_prepare(methods,&s));
    methods[0xac0/4]^=1; methods[0x1b0c/4]|=4; CHECK(nv2a_texture_copy_prepare(methods,&s));
    copy_methods(methods,3,2,8,8,2); CHECK(!nv2a_texture_copy_prepare(methods,&s));
    memset(target,0xcc,sizeof(target)); CHECK(DRAW());
    CHECK(!memcmp(texture,target,6)); CHECK(!memcmp(texture+8,target+8,6));
    CHECK(target[6]==0xcc && target[7]==0xcc); /* RGB565 direct-copy path honours pitch */
    /* Measured four-stage modulation: non-white diffuse must disable the
     * byte-copy shortcut and be interpolated with reciprocal W. */
    copy_methods(methods,3,2,8,20,4); modulate_methods(methods);
    CHECK(!nv2a_texture_copy_prepare(methods,&s) && s.modulate);
    vertices();
    for(int i=0;i<3;++i) for(int k=0;k<3;++k) v[i][3][k]=.25f*(k+1);
    CHECK(DRAW());
    CHECK(pixel(target)==0x48400000);
    CHECK((pixel(target+4)&0xffffff)==0x008000);
    CHECK((pixel(target+8)&0xffffff)==0x0000bf);
    for(int i=0;i<3;++i) for(int k=0;k<3;++k) v[i][3][k]=i==1 ? .75f : .25f;
    for(int i=0;i<3;++i) v[i][9][0]=v[i][9][1]=.5f; /* fixed red texel */
    v[1][0][3]=2;
    CHECK(DRAW()); CHECK(pixel(target)==0x44440000);
    vertices();
    for(int i=0;i<3;++i) { v[i][3][0]=2; v[i][3][1]=-1; v[i][3][2]=.5f; }
    CHECK(DRAW()); CHECK((pixel(target)&0xffffff)==0xff0000);
    CHECK((pixel(target+4)&0xffffff)==0);
    CHECK((pixel(target+8)&0xffffff)==0x000080);
    v[1][3][0]=NAN; memset(target,0xcc,sizeof(target));
    CHECK(!DRAW()); for(unsigned i=0;i<sizeof(target);++i) CHECK(target[i]==0xcc);
    vertices(); for(int i=0;i<3;++i) for(int k=0;k<3;++k) v[i][3][k]=.5f;
    copy_methods(methods,3,2,8,8,2); modulate_methods(methods);
    CHECK(!nv2a_texture_copy_prepare(methods,&s)); CHECK(DRAW());
    CHECK(target[0]==0 && target[1]==0x80); /* half red, not the original full red */
    s.dither=1; memset(target,0xcc,sizeof(target));
    CHECK(DRAW()); CHECK(target[0]==0 && target[1]==0x78); /* ordered half-red quantisation */
    /* Unsupported outputs and constant inputs still reject explicitly. */
    methods[0xaa0/4]=0xc01; CHECK(nv2a_texture_copy_prepare(methods,&s)); methods[0xaa0/4]=0xc00;
    methods[0xac0/4]=0x01200000; CHECK(nv2a_texture_copy_prepare(methods,&s)); methods[0xac0/4]=0x08040000;
    methods[0x300/4]=1; CHECK(!strcmp(nv2a_texture_copy_prepare(methods,&s),"alpha test"));
    methods[0x300/4]=0; methods[0x304/4]=1;
    CHECK(!strcmp(nv2a_texture_copy_prepare(methods,&s),"blending"));
    methods[0x304/4]=0; methods[0x1b04/4]=0x09920c29; /* two-level DXT1 mip chain */
    CHECK(!nv2a_texture_copy_prepare(methods,&s));
    CHECK(s.levels==2 && nv2a_texture_copy_texture_bytes(&s)==163840);
    puts("Texture DMA, RGB565, filtering, projection, modulation, alpha, bounds and rejection checks passed");
}
