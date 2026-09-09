#include "nv2a_metal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static void solid_dxt1(uint8_t *p,size_t bytes,uint16_t color)
{
    for(size_t i=0;i<bytes;i+=8){p[i]=color;p[i+1]=color>>8;p[i+2]=color;p[i+3]=color>>8;memset(p+i+4,0,4);}
}
int main(void)
{
    NV2ATextureCopy s={0};
    s.width=s.height=s.clip_w=s.clip_h=16;
    s.pitch=s.target_pitch=32;s.target_bpp=2;s.levels=1;s.texture_mask=1;
    uint8_t tex[512],cpu[512],gpu[512],zcpu[1024],zgpu[1024];
    for(unsigned i=0;i<sizeof(tex);++i) tex[i]=(unsigned char)(i*73+11);
    float v[3][16][4]={0};
    for(unsigned i=0;i<3;++i) v[i][0][3]=v[i][9][3]=1;
    v[1][0][0]=v[1][9][0]=32;v[2][0][1]=v[2][9][1]=32;
    for(unsigned shape=0;shape<2;++shape)
    for(unsigned dither=0;dither<2;++dither)
    for(unsigned linear=0;linear<2;++linear) {
        v[1][0][0]=v[1][9][0]=shape?20:32;
        v[2][0][1]=v[2][9][1]=shape?20:32;
        s.linear=linear;s.dither=dither;nv2a_metal_invalidate(gpu);
        memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
        CHECK(nv2a_texture_copy_triangle(&s,tex,sizeof(tex),cpu,sizeof(cpu),v[0],v[1],v[2]));
        int result=nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),NULL,0,v,3,5);
        if(result!=1)fprintf(stderr,"Metal rejected baseline draw: %s\n",nv2a_metal_last_reject());
        CHECK(result==1);
        CHECK(nv2a_metal_sync());
        CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    }
    s.depth_write=1;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle_depth(&s,tex,sizeof(tex),cpu,sizeof(cpu),NULL,0,v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    s.depth_test=1;s.depth_pitch=64;
    for(unsigned i=0;i<3;++i)v[i][0][2]=0x345678;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    for(unsigned i=0;i<sizeof(zcpu);i+=4){zcpu[i]=0x5a;zcpu[i+1]=0xff;zcpu[i+2]=0xff;zcpu[i+3]=0xff;}
    memcpy(zgpu,zcpu,sizeof(zcpu));
    CHECK(nv2a_texture_copy_triangle_depth(&s,tex,sizeof(tex),cpu,sizeof(cpu),zcpu,sizeof(zcpu),v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),zgpu,sizeof(zgpu),v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    CHECK(!memcmp(zcpu,zgpu,sizeof(zcpu)));
    s.depth_test=s.depth_write=0;s.depth_pitch=0;
    s.untextured=1;s.texture_mask=0;s.dither=1;s.width=s.height=s.pitch=0;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle(&s,NULL,0,cpu,sizeof(cpu),v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,NULL,0,gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    s.untextured=0;s.texture_mask=1;s.width=s.height=16;s.pitch=32;s.dither=0;
    s.blend=1;s.blend_src=0x302;s.blend_dst=0x303;nv2a_metal_invalidate(gpu);memset(gpu,0xcc,sizeof(gpu));memcpy(cpu,gpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle(&s,tex,sizeof(tex),cpu,sizeof(cpu),v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    s.blend=0;s.depth_pitch=64;s.stencil_test=s.stencil_write=1;s.stencil_mask=s.stencil_func_mask=0xff;
    s.stencil_func=0x207;s.stencil_ref=1;s.stencil_fail=s.stencil_zfail=0x1e00;s.stencil_zpass=0x1e02;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    for(unsigned i=0;i<sizeof(zcpu);i+=4){zcpu[i]=7;zcpu[i+1]=0xff;zcpu[i+2]=0xff;zcpu[i+3]=0xff;}
    memcpy(zgpu,zcpu,sizeof(zcpu));
    CHECK(nv2a_texture_copy_triangle_depth(&s,tex,sizeof(tex),cpu,sizeof(cpu),zcpu,sizeof(zcpu),v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),zgpu,sizeof(zgpu),v,3,5)==1);
    CHECK(nv2a_metal_sync());CHECK(!memcmp(cpu,gpu,sizeof(cpu)));CHECK(!memcmp(zcpu,zgpu,sizeof(zcpu)));
    s.stencil_test=s.stencil_write=0;s.depth_pitch=0;
    /* Exercise the BC2 path used by JSRF's first rejected city texture. */
    uint8_t bc2[256];
    for(unsigned i=0;i<sizeof(bc2);++i)bc2[i]=(unsigned char)(i*29+7);
    s.blend=0;s.dxt3=1;s.width=s.height=16;s.pitch=64;s.levels=1;s.linear=1;s.repeat=1;
    v[1][0][0]=v[2][0][1]=16;v[1][9][0]=v[2][9][1]=1;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle(&s,bc2,sizeof(bc2),cpu,sizeof(cpu),v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,bc2,sizeof(bc2),gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    s.dxt3=0;
    uint8_t mip[168],stage1[128];
    solid_dxt1(mip,128,0xf800);solid_dxt1(mip+128,32,0x07e0);solid_dxt1(mip+160,8,0x001f);
    s.blend=0;s.dxt1=1;s.width=s.height=16;s.pitch=32;s.levels=3;s.min_filter=5;s.linear=0;s.repeat=1;
    s.combiner_count=1;s.color_icw[0]=0x08200000;s.alpha_icw[0]=0x18200000;
    v[1][0][0]=v[2][0][1]=16;v[1][9][0]=v[2][9][1]=4;
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle_depth(&s,mip,sizeof(mip),cpu,sizeof(cpu),NULL,0,v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,mip,sizeof(mip),gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    NV2ATextureCopy extra[3]={{0}};
    solid_dxt1(mip,128,0xf800);solid_dxt1(stage1,sizeof(stage1),0x07e0);
    s.levels=1;s.texture_mask=3;s.combiner_count=1;s.color_icw[0]=0x09200000;s.alpha_icw[0]=0x19200000;s.extra_stages=extra;
    extra[0].dxt1=1;extra[0].width=extra[0].height=16;extra[0].pitch=32;extra[0].levels=1;extra[0].min_filter=1;extra[0].repeat=1;
    s.extra_texture[0]=stage1;s.extra_size[0]=sizeof(stage1);
    for(unsigned i=0;i<3;i++)memcpy(v[i][10],v[i][9],sizeof(v[i][10]));
    nv2a_metal_invalidate(gpu);memset(cpu,0xcc,sizeof(cpu));memcpy(gpu,cpu,sizeof(cpu));
    CHECK(nv2a_texture_copy_triangle_depth(&s,mip,128,cpu,sizeof(cpu),NULL,0,v[0],v[1],v[2]));
    CHECK(nv2a_metal_draw(&s,mip,128,gpu,sizeof(gpu),NULL,0,v,3,5)==1);
    CHECK(nv2a_metal_sync());
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    s.target_bpp=4;nv2a_metal_invalidate(gpu);memset(gpu,0xcc,sizeof(gpu));memcpy(cpu,gpu,sizeof(cpu));
    CHECK(nv2a_metal_draw(&s,tex,sizeof(tex),gpu,sizeof(gpu),NULL,0,v,3,5)==-1);
    CHECK(!memcmp(cpu,gpu,sizeof(cpu)));
    puts("Metal GPU RGB565, D24S8 stencil/depth, mipmap and multitexture paths match CPU; unsupported state preserves target");
    return 0;
}
