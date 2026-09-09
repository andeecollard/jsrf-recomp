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
    if ((M(0x288)!=0xc && M(0x288)!=0xe) || M(0x28c)!=0x1c80)
        return "combiner / texture program";
    if (M(0x1e60)<1 || M(0x1e60)>8) return "combiner / texture program";
    s->combiner_count=M(0x1e60); s->add_specular=M(0x288)==0xe;
    /* The measured programs write AB+CD to R0 with no dot/mux/output mapping.
     * Keep those more general output modes explicitly unsupported. */
    for (unsigned i=0;i<s->combiner_count;++i) {
        if (M(0xaa0+4*i)!=0xc00 || M(0x1e40+4*i)!=0xc00)
            return "combiner output mode";
        s->color_icw[i]=M(0xac0+4*i); s->alpha_icw[i]=M(0x260+4*i);
        for (unsigned j=0;j<4;++j) {
            unsigned regs[2]={(s->color_icw[i]>>(8*j))&15,(s->alpha_icw[i]>>(8*j))&15};
            for(unsigned k=0;k<2;++k)
                if (regs[k]!=0 && regs[k]!=4 && regs[k]!=5 &&
                        !(regs[k]>=8 && regs[k]<=12)) return "combiner input register";
        }
    }
    for (unsigned u=0;u<4;++u) {
        unsigned mode=(M(0x1e70)>>(5*u))&31;
        if (mode>1) return "texture shader mode";
        if (mode) {
            if (!(M(0x1b0c+64*u)&0x40000000)) return "disabled texture shader stage";
            s->texture_mask|=1u<<u;
        }
    }
    s->untextured=!(s->texture_mask&1); s->modulate=1;
    /* Retain the proven exact framebuffer-copy fast path. */
    if(s->texture_mask==1 && s->combiner_count==1 && !s->add_specular &&
            s->color_icw[0]==0x08200000 && s->alpha_icw[0]==0x14200000) {
        s->combiner_count=0; s->modulate=0;
    }
    if (M(0x300) && (M(0x300)!=1 || M(0x33c)!=0x204 || M(0x340)>255)) return "alpha test";
    s->alpha_test=M(0x300); s->alpha_ref=M(0x340);
    if (M(0x304)) {
        uint32_t src=M(0x344),dst=M(0x348);
        if (M(0x304)!=1 || (src!=0 && src!=1 && src!=0x302 && src!=0x303)
                || (dst!=0 && dst!=1 && dst!=0x302 && dst!=0x303)
                || M(0x350)!=0x8006) return "blending";
        s->blend=1;s->blend_src=src;s->blend_dst=dst;
    }
    if (M(0x308)) {
        if (M(0x308)!=1 || (M(0x39c)!=0x404 && M(0x39c)!=0x405 && M(0x39c)!=0x408)
                || (M(0x3a0)!=0x900 && M(0x3a0)!=0x901)) return "face culling";
        s->cull_face=M(0x39c); s->front_cw=M(0x3a0)==0x900;
    }
    if (M(0x30c) && (M(0x30c)!=1 || M(0x354)<0x200 || M(0x354)>0x207 || M(0x35c)>1
                || (M(0x290)&0x11000) || (M(0x208)&0xf0)!=0x20)) return "depth test";
    s->depth_test=M(0x30c); s->depth_write=M(0x35c)!=0; s->depth_func=M(0x354);
    s->depth_handle=M(0x198); s->depth_offset=M(0x214); s->depth_pitch=M(0x20c)>>16;
    if (M(0x32c)) {
        static const uint32_t operations[]={0,0x1e00,0x1e01,0x1e02,0x1e03,0x150a,0x8507,0x8508};
        uint32_t op[3]={M(0x370),M(0x374),M(0x378)};
        if(M(0x32c)!=1 || M(0x364)<0x200 || M(0x364)>0x207) return "stencil test";
        for(unsigned i=0;i<3;++i) {
            unsigned supported=0;
            for(unsigned j=0;j<sizeof(operations)/sizeof(operations[0]);++j) supported|=op[i]==operations[j];
            if(!supported) return "stencil operation";
        }
        s->stencil_test=1;s->stencil_write=(M(0x290)&1)!=0;s->stencil_mask=M(0x360);
        s->stencil_func=M(0x364);s->stencil_ref=M(0x368);s->stencil_func_mask=M(0x36c);
        s->stencil_fail=op[0];s->stencil_zfail=op[1];s->stencil_zpass=op[2];
    }
    if (M(0x2a4)) return "fog";
    if (M(0x324) || M(0x338)) return "polygon smoothing / offset";
    if (M(0x17bc)) return "logic op";
    if (M(0x37c)!=0x1d01 || M(0x38c)!=0x1b02 || M(0x390)!=0x1b02)
        return "shade / polygon mode";
    if (M(0x358)!=0x01010101 || M(0x2b4)) return "colour mask / window clip";
    if (s->untextured) goto target_state;
    { const char *error=nv2a_texture_copy_prepare_image(m,0,s); if(error) return error; }
target_state:
    s->dither=M(0x310)!=0;
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
    if ((s->depth_test || s->stencil_test) && s->depth_pitch<(s->clip_x+s->clip_w)*4u)
        return "depth dimensions / pitch";
    if ((M(0x2c0)&0xfff)>s->clip_x || ((M(0x2c0)>>16)&0xfff)<s->clip_x+s->clip_w-1
            || (M(0x2e0)&0xfff)>s->clip_y || ((M(0x2e0)>>16)&0xfff)<s->clip_y+s->clip_h-1)
        return "partial window clip";
    return NULL;
}
const char *nv2a_texture_copy_prepare_image(const uint32_t m[2048], unsigned unit, NV2ATextureCopy *s)
{
    if(unit>3) return "texture unit";
    unsigned b=0x1b00+64*unit;
    uint32_t control=M(b+12), f=M(b+4), dma=f&3, format=(f>>8)&255;
    if ((control&0xc000003fu)!=0x40000000u || (control&0x3ffc0000u))
        return "texture control / colour key / alpha kill";
    if ((f&0xf00000fcu)!=0x28 || (dma!=1 && dma!=2)) return "texture format / mip layout";
    s->levels=(f>>16)&15;
    if(!s->levels) return "texture mip levels";
    if(s->levels>1 && (control&0x3ffc0u)!=0x3ffc0u)
        return "texture maximum LOD clamp";
    if(format==0x11) {
        if(s->levels!=1) return "linear texture mip levels";
        s->width=M(b+28)>>16; s->height=M(b+28)&65535; s->pitch=M(b+16)>>16;
        if(!s->width || !s->height || s->pitch<s->width*2u) return "texture dimensions / pitch";
    } else if(format==0xc || format==0xe || format==6) {
        s->dxt1=format==0xc; s->dxt3=format==0xe; s->rgba8=format==6;
        unsigned lw=(f>>20)&15,lh=(f>>24)&15;
        if(lw>12 || lh>12 || s->levels>1+(lw>lh?lw:lh)) return "texture dimensions / pitch";
        s->width=1u<<lw; s->height=1u<<lh;
        s->pitch=s->dxt1 ? ((s->width+3)/4)*8 :
            s->dxt3 ? ((s->width+3)/4)*16 : s->width*4;
    } else return "texture format / mip layout";
    /* Repeat or clamp-to-edge; all three wrap components must agree. */
    if(M(b+8)==0x10101 && format!=0x11) s->repeat=1;
    else if(M(b+8)!=0x10303 && M(b+8)!=0x30303) return "texture address mode";
    uint32_t filter=M(b+20), min=(filter>>16)&255, mag=(filter>>24)&15;
    if((filter&0xf000e000)!=0x2000 || min<1 || min>6 || (mag!=1 && mag!=2)) return "texture filter / channel sign";
    s->linear=mag==2; s->min_filter=min;
    s->lod_bias=(float)((int32_t)((filter&8191)<<19)>>19)/256.0f;
    s->texture_handle=M(dma==1?0x184:0x188); s->texture_offset=M(b);
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

size_t nv2a_texture_copy_texture_bytes(const NV2ATextureCopy *s)
{
    if (s->untextured) return 0;
    size_t bytes=0;
    unsigned w=s->width,h=s->height;
    for(unsigned l=0;l<(s->levels?s->levels:1);++l) {
        bytes+=s->dxt1 ? (size_t)((w+3)/4)*((h+3)/4)*8 :
            s->dxt3 ? (size_t)((w+3)/4)*((h+3)/4)*16 :
            s->rgba8 ? (size_t)w*h*4 : (size_t)s->pitch*h;
        w=w>1?w/2:1; h=h>1?h/2:1;
    }
    return bytes;
}
static void unpack565(uint32_t v, float rgba[4])
{
    rgba[0]=(float)(v>>11)/31; rgba[1]=(float)((v>>5)&63)/63; rgba[2]=(float)(v&31)/31;
    rgba[3]=1;
}
static void texel(const NV2ATextureCopy *s, const uint8_t *data, int x, int y, float rgba[4])
{
    if (s->repeat) {
        x=(x%(int)s->width+(int)s->width)%(int)s->width;
        y=(y%(int)s->height+(int)s->height)%(int)s->height;
    } else {
        if (x<0) x=0; else if ((uint32_t)x>=s->width) x=(int)s->width-1;
        if (y<0) y=0; else if ((uint32_t)y>=s->height) y=(int)s->height-1;
    }
    if(s->rgba8) {
        unsigned index=0,bit=0;
        /* Rectangular Morton order: interleave only dimensions still active. */
        for(unsigned b=1;b<s->width || b<s->height;b<<=1) {
            if(b<s->width) { if((unsigned)x&b) index|=1u<<bit; ++bit; }
            if(b<s->height) { if((unsigned)y&b) index|=1u<<bit; ++bit; }
        }
        const uint8_t *p=data+4*(size_t)index;
        rgba[0]=p[2]/255.0f; rgba[1]=p[1]/255.0f; rgba[2]=p[0]/255.0f; rgba[3]=p[3]/255.0f;
        return;
    }
    if (s->dxt1 || s->dxt3) {
        /* BC1/BC2 consist of row-major 4x4 blocks, not Morton-swizzled texels. */
        const uint8_t *p=data+(size_t)(y/4)*s->pitch+(x/4)*(s->dxt3?16:8);
        float alpha=1;
        if(s->dxt3) {
            unsigned i=(unsigned)(y&3)*4+(unsigned)(x&3);
            alpha=(float)((p[i/2]>>(4*(i&1)))&15)/15;
            p+=8;
        }
        uint32_t c0=p[0]|(uint32_t)p[1]<<8,c1=p[2]|(uint32_t)p[3]<<8;
        unsigned index=(read32(p+4)>>(2*((y&3)*4+(x&3))))&3;
        if (index<2) { unpack565(index ? c1 : c0,rgba); rgba[3]=alpha; return; }
        if (!s->dxt3 && c0<=c1 && index==3) { memset(rgba,0,4*sizeof(float)); return; }
        float a[4],b[4]; unpack565(c0,a); unpack565(c1,b);
        float weight=!s->dxt3 && c0<=c1 ? .5f :
            (index==2 ? 2.0f/3.0f : 1.0f/3.0f);
        for (int k=0;k<3;++k) rgba[k]=a[k]*weight+b[k]*(1-weight);
        rgba[3]=alpha; return;
    }
    const uint8_t *p=data+(size_t)y*s->pitch+x*2;
    uint32_t v=p[0] | (uint32_t)p[1]<<8;
    unpack565(v,rgba);
}
static void sample(const NV2ATextureCopy *s, const uint8_t *data, float u, float v, float rgba[4])
{
    if (s->dxt1 || s->dxt3 || s->rgba8) {
        /* Nonlinear textures use normalized coordinates, unlike image rectangles. */
        if (s->repeat) { u-=floorf(u); v-=floorf(v); }
        else { u=fmaxf(0,fminf(1,u)); v=fmaxf(0,fminf(1,v)); }
        u*=s->width; v*=s->height;
    }
    /* Clamp before converting to integers, including extreme valid coordinates. */
    u=fmaxf(0, fminf((float)s->width,u));
    v=fmaxf(0, fminf((float)s->height,v));
    if (!s->linear) { texel(s,data,(int)floorf(u),(int)floorf(v),rgba); return; }
    float x=u-0.5f,y=v-0.5f, fx=floorf(x),fy=floorf(y), tx=x-fx,ty=y-fy;
    float p[4][4];
    texel(s,data,(int)fx,(int)fy,p[0]); texel(s,data,(int)fx+1,(int)fy,p[1]);
    texel(s,data,(int)fx,(int)fy+1,p[2]); texel(s,data,(int)fx+1,(int)fy+1,p[3]);
    for (int k=0;k<4;++k) rgba[k]=(p[0][k]*(1-tx)+p[1][k]*tx)*(1-ty)
                                      +(p[2][k]*(1-tx)+p[3][k]*tx)*ty;
}
static void sample_lod(const NV2ATextureCopy *s,const uint8_t *data,float u,float v,float lod,float out[4])
{
    float l=fmaxf(0,lod+s->lod_bias);
    if(s->min_filter<3 || s->levels<2) l=0;
    l=fminf(l,(float)(s->levels?s->levels-1:0));
    unsigned lo=(unsigned)(s->min_filter>=5?floorf(l):floorf(l+.5f));
    unsigned hi=s->min_filter>=5 && lo+1<s->levels?lo+1:lo;
    NV2ATextureCopy t=*s;
    if(lod+s->lod_bias>0 && s->min_filter) t.linear=(s->min_filter%2)==0;
    const uint8_t *p=data;
    float a[4],b[4];
    for(unsigned level=0;level<=hi;++level) {
        if(level==lo) sample(&t,p,u,v,a);
        if(level==hi) {
            /* A single selected mip was already sampled above. Keep the
             * interpolation arithmetic unchanged, but avoid decoding and
             * filtering the same texels twice for every fragment. */
            if(hi==lo) memcpy(b,a,sizeof(b));
            else sample(&t,p,u,v,b);
            break;
        }
        p+=t.dxt1?(size_t)((t.width+3)/4)*((t.height+3)/4)*8:
            t.dxt3?(size_t)((t.width+3)/4)*((t.height+3)/4)*16:(size_t)t.width*t.height*4;
        t.width=t.width>1?t.width/2:1; t.height=t.height>1?t.height/2:1;
        t.pitch=t.dxt1?((t.width+3)/4)*8:t.dxt3?((t.width+3)/4)*16:t.width*4;
    }
    float f=hi==lo?0:l-lo;
    for(unsigned k=0;k<4;++k) out[k]=a[k]*(1-f)+b[k]*f;
}

static float combiner_input(unsigned input,unsigned channel,const float regs[13][4])
{
    unsigned source=input&15;
    float x=regs[source][input&16?3:channel];
    switch(input>>5) {
    case 0:return fmaxf(0,x);
    case 1:return 1-fminf(1,fmaxf(0,x));
    case 2:return 2*fmaxf(0,x)-1;
    case 3:return 1-2*fmaxf(0,x);
    case 4:return fmaxf(0,x)-.5f;
    case 5:return .5f-fmaxf(0,x);
    case 6:return x;
    default:return -x;
    }
}

static void combine(const NV2ATextureCopy *s,float regs[13][4],float out[4])
{
    /* R0 alpha starts with texture zero's alpha on NV2A. */
    regs[12][3]=(s->texture_mask&1)?regs[8][3]:1;
    for(unsigned stage=0;stage<s->combiner_count;++stage) {
        float next[4];
        for(unsigned k=0;k<4;++k) {
            unsigned word=k==3?s->alpha_icw[stage]:s->color_icw[stage];
            unsigned channel=k==3?2:k; /* Alpha ICW selects blue or alpha. */
            float a=combiner_input(word>>24,channel,regs), b=combiner_input((word>>16)&255,channel,regs);
            float c=combiner_input((word>>8)&255,channel,regs),d=combiner_input(word&255,channel,regs);
            next[k]=fmaxf(-1,fminf(1,a*b+c*d));
        }
        memcpy(regs[12],next,sizeof(next));
    }
    for(unsigned k=0;k<4;++k)
        out[k]=fmaxf(0,fminf(1,regs[12][k]+(s->add_specular && k<3?regs[5][k]:0)));
}
static unsigned quantize(float value, unsigned max)
{
    return (unsigned)(fminf(1,fmaxf(0,value))*max+0.5f);
}
static int compare_value(uint32_t func,uint32_t source,uint32_t target)
{
    if(!func) func=0x203; /* Directly constructed test state defaults to LEQUAL. */
    switch(func) {
    case 0x200:return 0;case 0x201:return source<target;case 0x202:return source==target;
    case 0x203:return source<=target;case 0x204:return source>target;case 0x205:return source!=target;
    case 0x206:return source>=target;default:return 1;
    }
}
static uint8_t stencil_result(uint32_t op,uint8_t old,uint8_t ref)
{
    switch(op) {
    case 0:return 0;case 0x1e01:return ref;case 0x1e02:return old==255?255:(uint8_t)(old+1);
    case 0x1e03:return old?old-1:0;case 0x150a:return (uint8_t)~old;
    case 0x8507:return (uint8_t)(old+1);case 0x8508:return (uint8_t)(old-1);default:return old;
    }
}
static void stencil_update(const NV2ATextureCopy *s,uint8_t *zeta,uint32_t op)
{
    if(!s->stencil_write) return;
    uint8_t mask=(uint8_t)s->stencil_mask,old=zeta[0];
    uint8_t next=stencil_result(op,old,(uint8_t)s->stencil_ref);
    zeta[0]=(uint8_t)((old&~mask)|(next&mask));
}
static float blend_factor(uint32_t factor,float source_alpha)
{
    switch(factor) {case 0:return 0;case 1:return 1;case 0x302:return source_alpha;default:return 1-source_alpha;}
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
int nv2a_texture_copy_triangle_depth(const NV2ATextureCopy *s,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    uint8_t *depth, size_t depth_size,
    const float a[16][4], const float b[16][4], const float c[16][4])
{
    if (!target
            || (!s->untextured && (!texture || !s->width || !s->height
                || s->width>65535 || s->height>65535
                || s->pitch<(s->dxt1 ? ((s->width+3)/4)*8 :
                    s->dxt3 ? ((s->width+3)/4)*16 : s->width*2u)))
            || (s->target_bpp!=2 && s->target_bpp!=4)
            || s->target_pitch<(uint64_t)(s->clip_x+s->clip_w)*s->target_bpp
            || nv2a_texture_copy_texture_bytes(s)>texture_size
            || (uint64_t)s->target_pitch*(s->clip_y+s->clip_h)>target_size) return 0;
    if ((s->depth_test || s->stencil_test) && (!depth || s->depth_pitch<(uint64_t)(s->clip_x+s->clip_w)*4
                || (uint64_t)s->depth_pitch*(s->clip_y+s->clip_h)>depth_size)) return 0;
    for(unsigned unit=1;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
        if(!s->extra_stages || !s->extra_texture[unit-1] ||
                nv2a_texture_copy_texture_bytes(&s->extra_stages[unit-1])>s->extra_size[unit-1]) return 0;
    }
    const float (*v[3])[4]={a,b,c};
    for (int i=0;i<3;++i) {
        for (int k=0;k<4;++k)
            if (!isfinite(v[i][0][k])
                    || (!s->untextured && !isfinite(v[i][NV2A_VSH_OUT_T0][k])))
                return 0;
        if (!(v[i][0][3]>0) || !isfinite(v[i][NV2A_VSH_OUT_D0][3])
                || (!s->untextured && !(v[i][NV2A_VSH_OUT_T0][3]>0))) return 0;
        if (s->modulate)
            for (int k=0;k<3;++k)
                if (!isfinite(v[i][NV2A_VSH_OUT_D0][k])) return 0;
    }
    if(s->combiner_count) for(unsigned i=0;i<3;++i) {
        for(unsigned k=0;k<4;++k)
            if(!isfinite(v[i][NV2A_VSH_OUT_D0][k]) || !isfinite(v[i][NV2A_VSH_OUT_D1][k])) return 0;
        for(unsigned unit=0;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
            for(unsigned k=0;k<4;++k) if(!isfinite(v[i][NV2A_VSH_OUT_T0+unit][k])) return 0;
            if(!(v[i][NV2A_VSH_OUT_T0+unit][3]>0)) return 0;
        }
    }
    float area=edge(a[0],b[0],c[0][0],c[0][1]);
    if (!isfinite(area)) return 0;
    if (area==0) return 1;
    int front=(area>0)==(s->front_cw!=0); /* framebuffer Y increases downwards */
    if (s->cull_face==0x408 || (s->cull_face==0x404 && front)
            || (s->cull_face==0x405 && !front)) return 1;
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
    int direct=!s->combiner_count && !s->rgba8 && !s->modulate && !s->dxt1 && !s->dxt3 && !s->alpha_test && !s->blend && !s->depth_test && !s->stencil_test
        && s->target_bpp==2 && x0>=0 && y0>=0 && (uint32_t)x1<=s->width && (uint32_t)y1<=s->height;
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
        float u=0,t=0,q=0,alpha=0,recip=0,diffuse[3]={0};
        double z=0;
        /* Screen-space XYZ retain clip W in oPos.w. Perspective-correct
         * varyings use reciprocal W; PROJECT2D then divides S/T by Q.
         * The observed framebuffer copy has W=Q=1. */
        for (int i=0;i<3;++i) {
            float w=e[i]/area/v[i][0][3];
            z+=(double)e[i]/area*v[i][0][2]; /* post-viewport depth is affine */
            if (!s->untextured) {
                u+=w*v[i][NV2A_VSH_OUT_T0][0]; t+=w*v[i][NV2A_VSH_OUT_T0][1];
                q+=w*v[i][NV2A_VSH_OUT_T0][3];
            }
            alpha+=w*v[i][NV2A_VSH_OUT_D0][3]; recip+=w;
            if (s->modulate)
                for (int k=0;k<3;++k) diffuse[k]+=w*v[i][NV2A_VSH_OUT_D0][k];
        }
        if (!(recip>0)) return 0;
        float rgb[4];
        if(s->combiner_count) {
            float regs[13][4]={{0}};
            for(unsigned i=0;i<3;++i) {
                float w=e[i]/area/v[i][0][3]/recip;
                for(unsigned k=0;k<4;++k) {
                    regs[4][k]+=w*v[i][NV2A_VSH_OUT_D0][k];
                    regs[5][k]+=w*v[i][NV2A_VSH_OUT_D1][k];
                }
            }
            for(unsigned unit=0;unit<4;++unit) if(s->texture_mask&(1u<<unit)) {
                const NV2ATextureCopy *t=unit?&s->extra_stages[unit-1]:s;
                const uint8_t *data=unit?s->extra_texture[unit-1]:texture;
                float uv[3][2];
                for(unsigned at=0;at<3;++at) {
                    float sx=0,sy=0,sq=0;
                    float px=x+.5f+(at==1),py=y+.5f+(at==2);
                    float weights[3]={edge(b[0],c[0],px,py),edge(c[0],a[0],px,py),edge(a[0],b[0],px,py)};
                    for(unsigned i=0;i<3;++i) {
                        float w=weights[i]/area/v[i][0][3];
                        sx+=w*v[i][NV2A_VSH_OUT_T0+unit][0];
                        sy+=w*v[i][NV2A_VSH_OUT_T0+unit][1];
                        sq+=w*v[i][NV2A_VSH_OUT_T0+unit][3];
                    }
                    if(!isfinite(sq) || sq==0) return 0;
                    uv[at][0]=sx/sq;uv[at][1]=sy/sq;
                    if(!isfinite(uv[at][0]) || !isfinite(uv[at][1])) return 0;
                }
                float dx=hypotf((uv[1][0]-uv[0][0])*t->width,(uv[1][1]-uv[0][1])*t->height);
                float dy=hypotf((uv[2][0]-uv[0][0])*t->width,(uv[2][1]-uv[0][1])*t->height);
                float lod=log2f(fmaxf(0.000001f,fmaxf(dx,dy)));
                sample_lod(t,data,uv[0][0],uv[0][1],lod,regs[8+unit]);
            }
            combine(s,regs,rgb);
        } else {
            if (s->untextured) { rgb[0]=rgb[1]=rgb[2]=rgb[3]=1; }
            else {
                if (!(q>0) || !isfinite(u/q) || !isfinite(t/q)) return 0;
                sample(s,texture,u/q,t/q,rgb);
            }
            rgb[3]=fminf(1,fmaxf(0,alpha/recip))*(s->modulate ? rgb[3] : 1);
            if (s->modulate)
                for (int k=0;k<3;++k) rgb[k]=fminf(1,rgb[k]*fmaxf(0,diffuse[k]/recip));
        }
        if (s->alpha_test && quantize(rgb[3],255)<=s->alpha_ref) continue;
        uint8_t *zp=(s->depth_test||s->stencil_test) ? depth+(size_t)y*s->depth_pitch+x*4 : NULL;
        uint32_t z24=(uint32_t)(fmin(16777215,fmax(0,z))+.5);
        if(s->stencil_test) {
            uint8_t mask=(uint8_t)s->stencil_func_mask;
            if(!compare_value(s->stencil_func,(uint8_t)s->stencil_ref&mask,zp[0]&mask)) {
                stencil_update(s,zp,s->stencil_fail);continue;
            }
        }
        if (s->depth_test && !compare_value(s->depth_func,z24,read32(zp)>>8)) {
            if(s->stencil_test) stencil_update(s,zp,s->stencil_zfail);
            continue;
        }
        uint8_t *p=target+(size_t)y*s->target_pitch+x*s->target_bpp;
        if (s->blend) {
            float dst[4];
            if (s->target_bpp==2) unpack565(p[0]|(uint32_t)p[1]<<8,dst);
            else { dst[0]=p[2]/255.0f; dst[1]=p[1]/255.0f; dst[2]=p[0]/255.0f; dst[3]=p[3]/255.0f; }
            float source=blend_factor(s->blend_src,rgb[3]),destination=blend_factor(s->blend_dst,rgb[3]);
            for (int k=0;k<4;++k) rgb[k]=rgb[k]*source+dst[k]*destination;
        }
        if (s->dither && s->target_bpp==2) {
            /* Deterministic ordered approximation; NV2A's exact thresholds
             * still require hardware comparison. Preserve exact 565 levels. */
            static const unsigned bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
            float bias=((bayer[y&3][x&3]+.5f)/16.0f)-.5f;
            rgb[0]+=bias/31; rgb[1]+=bias/63; rgb[2]+=bias/31;
        }
        uint32_t pixel;
        if (s->target_bpp==2) pixel=quantize(rgb[0],31)<<11 | quantize(rgb[1],63)<<5 | quantize(rgb[2],31);
        else pixel=quantize(rgb[3],255)<<24 | quantize(rgb[0],255)<<16 | quantize(rgb[1],255)<<8 | quantize(rgb[2],255);
        for (unsigned k=0;k<s->target_bpp;++k) p[k]=(uint8_t)(pixel>>(8*k));
        if (zp && s->depth_write) { zp[1]=(uint8_t)z24; zp[2]=(uint8_t)(z24>>8); zp[3]=(uint8_t)(z24>>16); }
        if (s->stencil_test) stencil_update(s,zp,s->stencil_zpass);
    }
    return 1;
}

int nv2a_texture_copy_triangle(const NV2ATextureCopy *s,
    const uint8_t *texture, size_t texture_size, uint8_t *target, size_t target_size,
    const float a[16][4], const float b[16][4], const float c[16][4])
{
    return nv2a_texture_copy_triangle_depth(s,texture,texture_size,target,target_size,
                                          NULL,0,a,b,c);
}
