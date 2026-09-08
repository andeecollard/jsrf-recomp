#include "nv2a_texture_copy.h"
#include "texture_copy_state.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static uint32_t pixel(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int main(void)
{
    uint32_t m[2048]; NV2ATextureCopy s,extra[3]={{0}};
    uint8_t base[16]={0,0xf8,0,0,0,0,0,0, 0xe0,7,0,0,0,0,0,0};
    uint8_t light[64],target[64]={0}; float v[3][16][4]={0};
    copy_methods(m,4,4,8,16,4); modulate_methods(m);
    m[0x1b04/4]=0x02220c29; m[0x1b08/4]=0x10101; m[0x1b14/4]=0x01032000;
    m[0x1b44/4]=0x02210629; m[0x1b48/4]=0x30303;
    m[0x1b4c/4]=0x4003ffc0; m[0x1b54/4]=0x01012000;
    m[0x1e70/4]=0x21; m[0xac4/4]=0x090c0000; m[0x264/4]=0x0000201c;
    m[0x288/4]=0xe;
    CHECK(!nv2a_texture_copy_prepare(m,&s));
    CHECK(!nv2a_texture_copy_prepare_image(m,1,&extra[0]));
    CHECK(s.texture_mask==3 && extra[0].rgba8 && s.levels==2);
    CHECK(nv2a_texture_copy_texture_bytes(&s)==16);
    for(int i=0;i<16;++i) {light[i*4]=0;light[i*4+1]=255;light[i*4+2]=128;light[i*4+3]=128;}
    s.extra_stages=extra;s.extra_texture[0]=light;s.extra_size[0]=sizeof(light);
    for(int i=0;i<3;++i) {
        v[i][0][3]=v[i][9][3]=v[i][10][3]=1;
        v[i][9][0]=v[i][9][1]=v[i][10][0]=v[i][10][1]=.1f;
        for(int k=0;k<4;++k) v[i][3][k]=.5f;
        v[i][4][1]=.125f;
    }
    v[1][0][0]=8;v[2][0][1]=8;
#define DRAW() nv2a_texture_copy_triangle(&s,base,sizeof(base),target,sizeof(target),v[0],v[1],v[2])
    CHECK(DRAW()); CHECK(pixel(target)==0x80402000); /* T0 * diffuse * T1 + specular; alpha preserved. */
    v[1][9][0]=32;v[2][9][1]=32;
    CHECK(DRAW()); CHECK(pixel(target)==0x80009f00); /* Minification selects green mip. */
    s.extra_size[0]=4;memset(target,0xcc,sizeof(target));
    CHECK(!DRAW());CHECK(pixel(target)==0xcccccccc);
    s.extra_size[0]=sizeof(light);memset(light,0,sizeof(light));
    light[4*4+2]=64;light[4*4+3]=255; /* Morton index 4 is (2,0). */
    for(int i=0;i<3;++i) {
        v[i][9][0]=v[i][9][1]=.1f;
        v[i][10][0]=.625f;v[i][10][1]=.125f;v[i][4][1]=0;
    }
    CHECK(DRAW());CHECK(pixel(target)==0x80200000);
    m[0x1b0c/4]=0x40000000;
    CHECK(nv2a_texture_copy_prepare(m,&s)); /* Non-default max LOD is not silently ignored. */
    puts("Two textures, separate alpha, final specular, DXT1 mip selection, swizzle and bounds passed");
    return 0;
}
