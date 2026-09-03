/* NV2A field/combiner semantics: nv2a_regs.h and xemu pgraph/texture.c,
 * pgraph/glsl/psh.c. This implements the measured copy program, not a general
 * register-combiner interpreter. Keep unsupported states explicit. */
#include "nv2a_texture_copy.h"
#include "nv2a_vsh.h"
#include <math.h>
#include <string.h>

#define M(a) m[(a)/4]
const char *nv2a_texture_copy_prepare(const uint32_t m[2048], NV2ATextureCopy *s)
{
    memset(s, 0, sizeof(*s));
    if (!(M(0x1b0c) & 0x40000000)) return "texture 0 disabled";
    for (int u=1; u<4; ++u)
        if (M(0x1b0c+64*u) & 0x40000000) return "multiple textures";
    /* One PROJECT2D stage. A=T0, B=1, C=D=0 -> R0.rgb;
     * A=V0.alpha, B=1, C=D=0 -> R0.alpha. Final D/G select R0. */
    if (M(0x1e70)!=1 || M(0x1e60)!=1 || M(0xac0)!=0x08200000
            || M(0x260)!=0x14200000 || M(0xaa0)!=0xc00
            || M(0x1e40)!=0xc00 || M(0x288)!=0xc || M(0x28c)!=0x1c80)
        return "combiner / texture program";
    if (M(0x300) || M(0x304) || M(0x308) || M(0x30c) || M(0x32c) || M(0x2a4)
            || M(0x324) || M(0x338) || M(0x17bc))
        return "alpha / blend / cull / depth / stencil / fog / polygon / logic op";
    if (M(0x37c)!=0x1d01 || M(0x38c)!=0x1b02 || M(0x390)!=0x1b02)
        return "shade / polygon mode";
    if (M(0x358)!=0x01010101 || M(0x2b4)) return "colour mask / window clip";
    /* Observed control enables perspective, disables colour key and alpha kill.
     * The LOD range is immaterial for this single-level linear image. */
    if ((M(0x1b0c) & ~0x3ffffu)!=0x40000000 || (M(0x1b0c)&63))
        return "texture control / colour key / alpha kill";
    uint32_t f=M(0x1b04), dma=f&3;
    if ((f & 0xfffffffcu)!=0x00011128 || (dma!=1 && dma!=2))
        return "texture format / mip layout";
    if (M(0x1b08)!=0x00010303) return "texture address mode";
    uint32_t filter=M(0x1b14), min=(filter>>16)&255, mag=(filter>>24)&15;
    if ((filter & 0xf0000000) || (filter & 0x0000e000)!=0x2000)
        return "texture filter / channel sign";
    if (!((mag==1 && (min==1 || min==3 || min==5))
            || (mag==2 && (min==2 || min==4 || min==6))))
        return "texture min / mag filter";
    s->linear=mag==2;
    s->dither=M(0x310)!=0;
    s->texture_handle=M(dma==1 ? 0x184 : 0x188);
    s->texture_offset=M(0x1b00);
    s->width=M(0x1b1c)>>16; s->height=M(0x1b1c)&0xffff;
    s->pitch=M(0x1b10)>>16;
    if (!s->width || !s->height || s->pitch<s->width*2u)
        return "texture dimensions / pitch";
    uint32_t format=M(0x208);
    if (((format>>8)&15)!=1 || (format&0x0000f000)) return "nonlinear / multisample target";
    if ((format&15)==3) s->target_bpp=2;
    else if ((format&15)==8) s->target_bpp=4;
    else return "target colour format";
    s->target_handle=M(0x194); s->target_offset=M(0x210);
    s->target_pitch=M(0x20c)&0xffff;
    s->clip_x=M(0x200)&0xffff; s->clip_w=M(0x200)>>16;
    s->clip_y=M(0x204)&0xffff; s->clip_h=M(0x204)>>16;
    if (!s->clip_w || !s->clip_h
            || s->target_pitch<(s->clip_x+s->clip_w)*s->target_bpp)
        return "target dimensions / pitch";
    if ((M(0x2c0)&0xfff)>s->clip_x || ((M(0x2c0)>>16)&0xfff)<s->clip_x+s->clip_w-1
            || (M(0x2e0)&0xfff)>s->clip_y || ((M(0x2e0)>>16)&0xfff)<s->clip_y+s->clip_h-1)
        return "partial window clip";
    return NULL;
}
#undef M

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int nv2a_dma_resolve(const uint8_t *ramin, size_t size, uint32_t ramht,
                     uint32_t handle, uint32_t *base, uint32_t *limit)
{
    size_t start=((ramht>>4)&31)<<12, bytes=4096u<<((ramht>>16)&3);
    if (!ramin || start>size || bytes>size-start || !handle) return 0;
    for (size_t off=start; off<start+bytes; off+=8) {
        uint32_t context=read32(ramin+off+4);
        /* The CPU method sink currently runs channel zero only. */
        if (read32(ramin+off)!=handle || !(context&0x80000000)
                || (context&0x1f000000) || (context&0x00020000)) continue;
        size_t instance=(context&0xffff)<<4;
        if (instance>size || 16>size-instance) return 0;
        uint32_t flags=read32(ramin+instance), frame=read32(ramin+instance+8);
        if ((flags&0xfff)!=0x3d || (flags&0x30000)) return 0;
        *base=((frame&0xfffff000) | (flags>>20)) & 0x07ffffff;
        *limit=read32(ramin+instance+4);
        return 1;
    }
    return 0;
}

static void texel(const NV2ATextureCopy *s, const uint8_t *data, int x, int y, float rgb[3])
{
    if (x<0) x=0; else if ((uint32_t)x>=s->width) x=(int)s->width-1;
    if (y<0) y=0; else if ((uint32_t)y>=s->height) y=(int)s->height-1;
    const uint8_t *p=data+(size_t)y*s->pitch+x*2;
    uint32_t v=p[0] | (uint32_t)p[1]<<8;
    rgb[0]=(float)(v>>11)/31; rgb[1]=(float)((v>>5)&63)/63; rgb[2]=(float)(v&31)/31;
}
static void sample(const NV2ATextureCopy *s, const uint8_t *data, float u, float v, float rgb[3])
{
    /* Clamp before converting to integers, including extreme valid coordinates. */
    u=fmaxf(0, fminf((float)s->width,u));
    v=fmaxf(0, fminf((float)s->height,v));
    if (!s->linear) { texel(s,data,(int)floorf(u),(int)floorf(v),rgb); return; }
    float x=u-0.5f,y=v-0.5f, fx=floorf(x),fy=floorf(y), tx=x-fx,ty=y-fy;
    float p[4][3];
    texel(s,data,(int)fx,(int)fy,p[0]); texel(s,data,(int)fx+1,(int)fy,p[1]);
    texel(s,data,(int)fx,(int)fy+1,p[2]); texel(s,data,(int)fx+1,(int)fy+1,p[3]);
    for (int k=0;k<3;++k) rgb[k]=(p[0][k]*(1-tx)+p[1][k]*tx)*(1-ty)
                                      +(p[2][k]*(1-tx)+p[3][k]*tx)*ty;
}
static unsigned quantize(float value, unsigned max)
{
    return (unsigned)(fminf(1,fmaxf(0,value))*max+0.5f);
}
static float edge(const float a[4], const float b[4], float x, float y)
{
    return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);
}
static int inclusive_edge(const float a[4], const float b[4], float sign)
{
    float dy=(b[1]-a[1])*sign, dx=(b[0]-a[0])*sign;
    return dy<0 || (dy==0 && dx>0);
}
int nv2a_texture_copy_triangle(const NV2ATextureCopy *s,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    const float a[16][4], const float b[16][4], const float c[16][4])
{
    if (!texture || !target || !s->width || !s->height || s->pitch<s->width*2u
            || (s->target_bpp!=2 && s->target_bpp!=4)
            || s->target_pitch<(uint64_t)(s->clip_x+s->clip_w)*s->target_bpp
            || (uint64_t)s->pitch*s->height>texture_size
            || (uint64_t)s->target_pitch*(s->clip_y+s->clip_h)>target_size) return 0;
    const float (*v[3])[4]={a,b,c};
    for (int i=0;i<3;++i) {
        for (int k=0;k<4;++k)
            if (!isfinite(v[i][0][k]) || !isfinite(v[i][NV2A_VSH_OUT_T0][k])) return 0;
        if (!(v[i][0][3]>0) || !(v[i][NV2A_VSH_OUT_T0][3]>0)
                || !isfinite(v[i][NV2A_VSH_OUT_D0][3])) return 0;
        /* Dithering cannot change an exact RGB565 -> RGB565 texel copy.
         * Other dithered conversions need hardware-verified quantisation. */
        if (s->dither && (s->target_bpp!=2 || v[i][0][3]!=1
                || v[i][NV2A_VSH_OUT_T0][3]!=1
                || v[i][NV2A_VSH_OUT_T0][0]!=v[i][0][0]
                || v[i][NV2A_VSH_OUT_T0][1]!=v[i][0][1])) return 0;
    }
    float area=edge(a[0],b[0],c[0][0],c[0][1]);
    if (!isfinite(area)) return 0;
    if (area==0) return 1;
    float left=(float)s->clip_x,right=(float)(s->clip_x+s->clip_w);
    float top=(float)s->clip_y,bottom=(float)(s->clip_y+s->clip_h);
    int x0=(int)floorf(fmaxf(left,fminf(right,fminf(a[0][0],fminf(b[0][0],c[0][0])))));
    int x1=(int)ceilf(fmaxf(left,fminf(right,fmaxf(a[0][0],fmaxf(b[0][0],c[0][0])))));
    int y0=(int)floorf(fmaxf(top,fminf(bottom,fminf(a[0][1],fminf(b[0][1],c[0][1])))));
    int y1=(int)ceilf(fmaxf(top,fminf(bottom,fmaxf(a[0][1],fmaxf(b[0][1],c[0][1])))));
    float sign=area>0 ? 1.0f : -1.0f;
    int include[3]={inclusive_edge(b[0],c[0],sign),inclusive_edge(c[0],a[0],sign),inclusive_edge(a[0],b[0],sign)};
    /* A common post-process is an oversized triangle copying texels 1:1.
     * Prove that all pixel centres in its bounds are covered before using
     * row copies. This also preserves RGB565 quantisation exactly. */
    int direct=s->target_bpp==2 && x0>=0 && y0>=0 && (uint32_t)x1<=s->width && (uint32_t)y1<=s->height;
    for (int i=0;i<3;++i)
        if (v[i][0][3]!=1 || v[i][NV2A_VSH_OUT_T0][3]!=1
                || v[i][0][0]!=v[i][NV2A_VSH_OUT_T0][0]
                || v[i][0][1]!=v[i][NV2A_VSH_OUT_T0][1]) direct=0;
    for (int corner=0;direct && corner<4;++corner) {
        float px=(corner&1) ? x1-.5f : x0+.5f, py=(corner&2) ? y1-.5f : y0+.5f;
        float e[3]={edge(b[0],c[0],px,py),edge(c[0],a[0],px,py),edge(a[0],b[0],px,py)};
        for (int i=0;i<3;++i) if (e[i]*sign<0 || (e[i]==0 && !include[i])) direct=0;
    }
    if (direct && x1>x0 && y1>y0) {
        for (int y=y0;y<y1;++y)
            memcpy(target+(size_t)y*s->target_pitch+x0*2, texture+(size_t)y*s->pitch+x0*2, (size_t)(x1-x0)*2);
        return 1;
    }
    for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
        float e[3]={edge(b[0],c[0],x+.5f,y+.5f),edge(c[0],a[0],x+.5f,y+.5f),edge(a[0],b[0],x+.5f,y+.5f)};
        int inside=1;
        for (int i=0;i<3;++i) if (e[i]*sign<0 || (e[i]==0 && !include[i])) inside=0;
        if (!inside) continue;
        float u=0,t=0,q=0,alpha=0,recip=0;
        /* Screen-space XYZ retain clip W in oPos.w. Perspective-correct
         * varyings use reciprocal W; PROJECT2D then divides S/T by Q.
         * The observed framebuffer copy has W=Q=1. */
        for (int i=0;i<3;++i) {
            float w=e[i]/area/v[i][0][3];
            u+=w*v[i][NV2A_VSH_OUT_T0][0]; t+=w*v[i][NV2A_VSH_OUT_T0][1];
            q+=w*v[i][NV2A_VSH_OUT_T0][3];
            alpha+=w*v[i][NV2A_VSH_OUT_D0][3]; recip+=w;
        }
        if (!(q>0) || !(recip>0) || !isfinite(u/q) || !isfinite(t/q)) return 0;
        float rgb[3]; sample(s,texture,u/q,t/q,rgb);
        uint32_t pixel;
        if (s->target_bpp==2) pixel=quantize(rgb[0],31)<<11 | quantize(rgb[1],63)<<5 | quantize(rgb[2],31);
        else pixel=quantize(alpha/recip,255)<<24 | quantize(rgb[0],255)<<16 | quantize(rgb[1],255)<<8 | quantize(rgb[2],255);
        uint8_t *p=target+(size_t)y*s->target_pitch+x*s->target_bpp;
        for (unsigned k=0;k<s->target_bpp;++k) p[k]=(uint8_t)(pixel>>(8*k));
    }
    return 1;
}
