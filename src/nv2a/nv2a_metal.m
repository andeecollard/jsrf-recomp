#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nv2a_metal.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Native raster path for the fragment states already understood by the CPU
 * renderer. Surfaces stay on the GPU across compatible batches. Guest RAM is
 * synchronized only at a flip, clear, diagnostic capture, or software fallback. */
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;
static id<MTLTexture> surface,stencil_surface;
static id<MTLCommandBuffer> last_command;
static uint8_t *surface_target,*depth_target;
static size_t surface_target_size,depth_target_size;
static uint32_t surface_width,surface_height,surface_pitch,depth_pitch;
static int surface_valid,surface_dirty,depth_valid,depth_dirty,attempted;
static pthread_mutex_t initialization_mutex=PTHREAD_MUTEX_INITIALIZER;
static const char *reject_reason;

#define TEXTURE_CACHE_SIZE 128
typedef struct {
    const uint8_t *source;
    size_t size;
    id<MTLBuffer> buffer;
    uint64_t stamp;
} TextureBuffer;
static TextureBuffer texture_cache[TEXTURE_CACHE_SIZE];
static id<MTLBuffer> dummy_buffer;
static uint64_t texture_clock,texture_requests,texture_hits,texture_uploads;
static uint64_t inline_vertex_batches,allocated_vertex_batches;

static NSString *const shader =
@"#include <metal_stdlib>\n"
 "using namespace metal;\n"
 "struct Vertex { float4 p,d0,d1,t0,t1,t2,t3; };\n"
 "struct Params { uint width,height,dither,untextured,combiner_count,texture_mask,add_specular,alpha_test,alpha_ref,modulate,blend,blend_src,blend_dst,depth_test,depth_write,depth_func;"
 " uint stencil_test,stencil_write,stencil_mask,stencil_ref,stencil_func_mask,stencil_func,stencil_fail,stencil_zfail,stencil_zpass;"
 " uint tw[4],th[4],pitch[4],linear[4],rgba8[4],dxt1[4],dxt3[4],repeat[4],levels[4],min_filter[4]; float lod_bias[4];"
 " uint color_icw[8]; uint alpha_icw[8]; uint color_ocw[8]; uint alpha_ocw[8]; };\n"
 "struct Out { float4 p [[position]]; float4 d0,d1,t0,t1,t2,t3; };\n"
 "struct Frag { float4 color [[color(0)]]; uint stencil [[color(1)]]; };\n"
 "vertex Out vs(uint id [[vertex_id]], const device Vertex *v [[buffer(0)]], constant Params &s [[buffer(1)]],const device uint*indices [[buffer(2)]]) {\n"
 " Vertex x=v[indices[id]];Out o;float4 p=x.p;float z=clamp(p.z,0.0f,16777215.0f)/16777215.0f;"
 " o.p=float4((p.x/s.width*2-1)*p.w,(1-p.y/s.height*2)*p.w,z*p.w,p.w);\n"
 " o.d0=x.d0;o.d1=x.d1;o.t0=x.t0;o.t1=x.t1;o.t2=x.t2;o.t3=x.t3;return o; }\n"
 "uint morton(uint x,uint y,uint w,uint h) { uint index=0,bit=0;"
 " for(uint b=1;b<w||b<h;b<<=1) { if(b<w){if(x&b)index|=1u<<bit;bit++;}"
 " if(b<h){if(y&b)index|=1u<<bit;bit++;}} return index; }\n"
 "float4 texel(const device uchar *t,int2 p,uint u,uint base,uint w,uint h,uint pitch,constant Params&s){\n"
 " if(s.repeat[u]){p.x=(p.x%int(w)+int(w))%int(w);p.y=(p.y%int(h)+int(h))%int(h);}"
 " else p=clamp(p,int2(0),int2(w-1,h-1));"
 " uint at=base+(s.rgba8[u]?4*morton(uint(p.x),uint(p.y),w,h):s.dxt1[u]?uint(p.y/4)*pitch+uint(p.x/4)*8:s.dxt3[u]?uint(p.y/4)*pitch+uint(p.x/4)*16:uint(p.y)*pitch+uint(p.x)*2);\n"
 " if(s.rgba8[u])return float4(float(t[at+2]),float(t[at+1]),float(t[at]),float(t[at+3]))/255;"
 " if(s.dxt1[u]){uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);"
 " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*((p.y&3)*4+(p.x&3)))&3;"
 " uint c=pick?c1:c0;float4 a=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);if(pick<2)return a;"
 " if(c0<=c1&&pick==3)return float4(0);c=c0;float4 x=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);"
 " c=c1;float4 y=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);float w=c0<=c1?.5f:(pick==2?2.0f/3.0f:1.0f/3.0f);return float4(x.rgb*w+y.rgb*(1-w),1);}"
 " if(s.dxt3[u]){uint i=uint(p.y&3)*4+uint(p.x&3),a=(uint(t[at+i/2])>>(4*(i&1)))&15;at+=8;"
 " uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);"
 " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*i)&3;"
 " uint c=pick?c1:c0;float3 x=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);if(pick<2)return float4(x,float(a)/15);"
 " c=c0;float3 r=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);c=c1;float3 b=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);"
 " float w=pick==2?2.0f/3.0f:1.0f/3.0f;return float4(r*w+b*(1-w),float(a)/15);}"
 " uint c=uint(t[at])|(uint(t[at+1])<<8);return float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);}\n"
 "float4 sample_level(const device uchar*t,float2 uv,uint u,uint level,bool linear,constant Params&s){"
 " uint base=0,w=s.tw[u],h=s.th[u],pitch=s.pitch[u];for(uint l=0;l<level;l++){"
 " base+=s.dxt1[u]?((w+3)/4)*((h+3)/4)*8:s.dxt3[u]?((w+3)/4)*((h+3)/4)*16:s.rgba8[u]?w*h*4:pitch*h;w=max(1u,w/2);h=max(1u,h/2);pitch=s.dxt1[u]?((w+3)/4)*8:s.dxt3[u]?((w+3)/4)*16:s.rgba8[u]?w*4:pitch;}"
 " if(s.rgba8[u]||s.dxt1[u]||s.dxt3[u]){if(s.repeat[u])uv-=floor(uv);else uv=clamp(uv,float2(0),float2(1));uv*=float2(w,h);}"
 " uv=clamp(uv,float2(0),float2(w,h));if(!linear)return texel(t,int2(floor(uv)),u,base,w,h,pitch,s);"
 " float2 p=uv-.5f,f=floor(p),fxy=p-f;int2 q=int2(f);"
 " return(texel(t,q,u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,0),u,base,w,h,pitch,s)*fxy.x)*(1-fxy.y)"
 " +(texel(t,q+int2(0,1),u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,1),u,base,w,h,pitch,s)*fxy.x)*fxy.y;}\n"
 "float4 sample_lod(const device uchar*t,float4 tc,uint u,constant Params&s){float2 uv=tc.xy/tc.w;"
 " float2 scale=float2(s.tw[u],s.th[u]);float lod=log2(max(0.000001f,max(length(dfdx(uv)*scale),length(dfdy(uv)*scale))));"
 " float l=max(0.0f,lod+s.lod_bias[u]);if(s.min_filter[u]<3||s.levels[u]<2)l=0;"
 " l=min(l,float(s.levels[u]-1));uint lo=s.min_filter[u]>=5?uint(floor(l)):uint(floor(l+.5f));"
 " uint hi=s.min_filter[u]>=5&&lo+1<s.levels[u]?lo+1:lo;bool linear=s.linear[u]!=0;"
 " if(lod+s.lod_bias[u]>0&&s.min_filter[u])linear=(s.min_filter[u]&1)==0;"
 " float4 a=sample_level(t,uv,u,lo,linear,s),b=hi==lo?a:sample_level(t,uv,u,hi,linear,s);"
 " return mix(a,b,hi==lo?0.0f:l-float(lo));}\n"
 "float input(uint code,uint channel,thread float4 *r){uint source=code&15;float x=r[source][(code&16)?3:channel];"
 " switch(code>>5){case 0:return max(0.0f,x);case 1:return 1-min(1.0f,max(0.0f,x));"
 " case 2:return 2*max(0.0f,x)-1;case 3:return 1-2*max(0.0f,x);"
 " case 4:return max(0.0f,x)-.5f;case 5:return .5f-max(0.0f,x);case 6:return x;default:return-x;}}\n"
 "bool cmpf(uint f,float a,float b){if(!f)f=0x203;switch(f){case 0x200:return false;case 0x201:return a<b;case 0x202:return a==b;case 0x203:return a<=b;case 0x204:return a>b;case 0x205:return a!=b;case 0x206:return a>=b;default:return true;}}\n"
 "bool cmpu(uint f,uint a,uint b){switch(f){case 0x200:return false;case 0x201:return a<b;case 0x202:return a==b;case 0x203:return a<=b;case 0x204:return a>b;case 0x205:return a!=b;case 0x206:return a>=b;default:return true;}}\n"
 "uint stop(uint op,uint old,uint ref){switch(op){case 0:return 0;case 0x1e01:return ref;case 0x1e02:return min(255u,old+1);case 0x1e03:return old?old-1:0;case 0x150a:return old^255;case 0x8507:return(old+1)&255;case 0x8508:return(old-1)&255;default:return old;}}\n"
 "uint stupd(uint old,uint op,constant Params&s){if(!s.stencil_write)return old;uint mask=s.stencil_mask&255,n=stop(op,old,s.stencil_ref&255);return(old&~mask)|(n&mask);}\n"
 "float bfactor(uint f,float a){switch(f){case 0:return 0;case 1:return 1;case 0x302:return a;default:return 1-a;}}\n"
 "fragment Frag fs(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],uint stencil [[color(1),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]]){\n"
 " Frag o;o.color=dst;o.stencil=stencil;float4 d0=i.d0,d1=i.d1,c=float4(1),tex=float4(0);"
 " if(s.texture_mask&1)tex=sample_lod(t0,i.t0,0,s);"
 " if(s.combiner_count){float4 r[14];for(uint n=0;n<14;n++)r[n]=float4(0);r[4]=d0;r[5]=d1;r[8]=tex;"
 " if(s.texture_mask&2)r[9]=sample_lod(t1,i.t1,1,s);if(s.texture_mask&4)r[10]=sample_lod(t2,i.t2,2,s);if(s.texture_mask&8)r[11]=sample_lod(t3,i.t3,3,s);"
 " r[12].a=(s.texture_mask&1)?r[8].a:1;for(uint stage=0;stage<s.combiner_count;stage++){float4 ab,cd;"
 " for(uint k=0;k<4;k++){uint word=k==3?s.alpha_icw[stage]:s.color_icw[stage];uint ch=k==3?2:k;"
 " float a=input(word>>24,ch,r),b=input((word>>16)&255,ch,r),cc=input((word>>8)&255,ch,r),d=input(word&255,ch,r);"
 " ab[k]=a*b;cd[k]=cc*d;}for(uint k=0;k<4;k++){uint word=k==3?s.alpha_ocw[stage]:s.color_ocw[stage];"
 " uint dd=word&15,da=(word>>4)&15,ds=(word>>8)&15;if(dd)r[dd][k]=clamp(cd[k],-1.0f,1.0f);"
 " if(da)r[da][k]=clamp(ab[k],-1.0f,1.0f);if(ds)r[ds][k]=clamp(ab[k]+cd[k],-1.0f,1.0f);}}"
 " c=clamp(r[12]+(s.add_specular?float4(r[5].rgb,0):float4(0)),0.0f,1.0f);}"
 " else if(!s.untextured){c=tex;c.a=clamp(d0.a,0.0f,1.0f)*(s.modulate?c.a:1);if(s.modulate)c.rgb*=max(float3(0),d0.rgb);}"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref)return o;"
 " uint mask=s.stencil_func_mask&255;if(s.stencil_test&&!cmpu(s.stencil_func,s.stencil_ref&mask,stencil&mask)){o.stencil=stupd(stencil,s.stencil_fail,s);return o;}"
 " if(s.depth_test&&!cmpf(s.depth_func,i.p.z,dst.a)){if(s.stencil_test)o.stencil=stupd(stencil,s.stencil_zfail,s);return o;}"
 " if(s.blend){float a=c.a;c=c*bfactor(s.blend_src,a)+dst*bfactor(s.blend_dst,a);}"
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}o.color=float4(c.rgb,s.depth_write?i.p.z:dst.a);if(s.stencil_test)o.stencil=stupd(stencil,s.stencil_zpass,s);return o;}\n";

static int initialize(void)
{
    pthread_mutex_lock(&initialization_mutex);
    if(attempted){int ready=pipeline!=nil;pthread_mutex_unlock(&initialization_mutex);return ready;}
    device=MTLCreateSystemDefaultDevice();if(!device){attempted=1;pthread_mutex_unlock(&initialization_mutex);return 0;}
    NSError *error=nil;MTLCompileOptions *options=[MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if(@available(macOS 15.0,*)) options.mathMode=MTLMathModeSafe; else options.fastMathEnabled=NO;
#pragma clang diagnostic pop
    id<MTLLibrary> library=[device newLibraryWithSource:shader options:options error:&error];
    if(library){MTLRenderPipelineDescriptor *desc=[MTLRenderPipelineDescriptor new];
        desc.vertexFunction=[library newFunctionWithName:@"vs"];desc.fragmentFunction=[library newFunctionWithName:@"fs"];
        desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
        desc.colorAttachments[1].pixelFormat=MTLPixelFormatR8Uint;
        pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
        queue=[device newCommandQueue];}
    attempted=1;
    if(!pipeline||!queue){pipeline=nil;fprintf(stderr,"[METAL] initialization failed: %s\n",error.description.UTF8String);pthread_mutex_unlock(&initialization_mutex);return 0;}
    fprintf(stderr,"[METAL] native raster pipeline ready: %s\n",device.name.UTF8String);pthread_mutex_unlock(&initialization_mutex);return 1;
}

static id<MTLBuffer> texture_buffer(const uint8_t *data,size_t size)
{
    if(!size) {
        if(!dummy_buffer){uint8_t zero=0;dummy_buffer=[device newBufferWithBytes:&zero length:1 options:MTLResourceStorageModeShared];}
        return dummy_buffer;
    }
    ++texture_requests;
    TextureBuffer *slot=NULL,*oldest=&texture_cache[0];
    for(unsigned i=0;i<TEXTURE_CACHE_SIZE;i++) {
        TextureBuffer *entry=&texture_cache[i];
        if(entry->source==data&&entry->size==size) {
            slot=entry;
            if(entry->buffer&&!memcmp(entry->buffer.contents,data,size)) {
                entry->stamp=++texture_clock;++texture_hits;return entry->buffer;
            }
            break;
        }
        if(!entry->buffer){if(!slot)slot=entry;}
        else if(entry->stamp<oldest->stamp)oldest=entry;
    }
    if(!slot)slot=oldest;
    id<MTLBuffer> buffer=[device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    if(!buffer)return nil;
    slot->source=data;slot->size=size;slot->buffer=buffer;slot->stamp=++texture_clock;++texture_uploads;
    return buffer;
}

void nv2a_metal_report(void)
{
    fprintf(stderr,"[METAL] texture buffers: %llu requests, %llu cache hits, %llu uploads; vertices: %llu inline, %llu allocated\n",
        (unsigned long long)texture_requests,(unsigned long long)texture_hits,
        (unsigned long long)texture_uploads,(unsigned long long)inline_vertex_batches,
        (unsigned long long)allocated_vertex_batches);
}

typedef struct{float p[4],d0[4],d1[4],t[4][4];}Vertex;
typedef struct{uint32_t width,height,dither,untextured,combiner_count,texture_mask,add_specular,alpha_test,alpha_ref,modulate,blend,blend_src,blend_dst,depth_test,depth_write,depth_func;
    uint32_t stencil_test,stencil_write,stencil_mask,stencil_ref,stencil_func_mask,stencil_func,stencil_fail,stencil_zfail,stencil_zpass;
    uint32_t tw[4],th[4],pitch[4],linear[4],rgba8[4],dxt1[4],dxt3[4],repeat[4],levels[4],min_filter[4];
    float lod_bias[4];
    uint32_t color_icw[8],alpha_icw[8],color_ocw[8],alpha_ocw[8];}Params;

int nv2a_metal_sync(void)
{
    @autoreleasepool{
        if(!surface_dirty&&!depth_dirty)return 1;
        [last_command waitUntilCompleted];
        if(last_command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"[METAL] command failed: %s\n",last_command.error.description.UTF8String);return 0;}
        size_t pixels=(size_t)surface_width*surface_height;
        float *rgba=malloc(pixels*16);
        if(!rgba)return 0;
        [surface getBytes:rgba bytesPerRow:surface_width*16 fromRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0];
        if(surface_dirty) {
            for(unsigned y=0;y<surface_height;++y) for(unsigned x=0;x<surface_width;++x) {
                size_t at=((size_t)y*surface_width+x)*4;
                unsigned c=(unsigned)(fminf(1,fmaxf(0,rgba[at]))*31+.5f)<<11|(unsigned)(fminf(1,fmaxf(0,rgba[at+1]))*63+.5f)<<5|(unsigned)(fminf(1,fmaxf(0,rgba[at+2]))*31+.5f);
                uint8_t*p=surface_target+(size_t)y*surface_pitch+x*2;
                p[0]=c;p[1]=c>>8;
            }
            surface_dirty=0;
        }
        if(depth_dirty&&depth_target) {
            uint8_t *stencil=malloc(pixels);
            if(!stencil){free(rgba);return 0;}
            [stencil_surface getBytes:stencil bytesPerRow:surface_width fromRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0];
            for(unsigned y=0;y<surface_height;++y) for(unsigned x=0;x<surface_width;++x) {
                size_t at=((size_t)y*surface_width+x)*4;
                uint32_t q=(uint32_t)((double)fminf(1,fmaxf(0,rgba[at+3]))*16777215.0+0.5);
                uint8_t*p=depth_target+(size_t)y*depth_pitch+x*4;
                p[0]=stencil[(size_t)y*surface_width+x];p[1]=q;p[2]=q>>8;p[3]=q>>16;
            }
            free(stencil);
            depth_dirty=0;
        }
        free(rgba);
        return 1;}
}

void nv2a_metal_invalidate(uint8_t *target)
{if(!target||target==surface_target||target==depth_target){nv2a_metal_sync();surface_valid=depth_valid=0;}}
const char *nv2a_metal_last_reject(void){return reject_reason?reject_reason:"none";}
static int reject(const char *reason){reject_reason=reason;nv2a_metal_sync();surface_valid=depth_valid=0;return-1;}
static float area(const float a[4],const float b[4],const float c[4])
{return(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);}
static int vertex_valid(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned i)
{for(unsigned k=0;k<4;k++)if(!isfinite(v[i][0][k])||!isfinite(v[i][3][k])||!isfinite(v[i][4][k]))return 0;
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){for(unsigned k=0;k<4;k++)if(!isfinite(v[i][9+u][k]))return 0;if(v[i][9+u][3]<=0)return 0;}return 1;}
static void triangle(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned*out,unsigned*n,unsigned a,unsigned b,unsigned c)
{if(!vertex_valid(s,v,a)||!vertex_valid(s,v,b)||!vertex_valid(s,v,c))return;float ar=area(v[a][0],v[b][0],v[c][0]);if(!isfinite(ar)||ar==0)return;int front=(ar>0)==(s->front_cw!=0);
 if(s->cull_face==0x408||(s->cull_face==0x404&&front)||(s->cull_face==0x405&&!front))return;out[(*n)++]=a;out[(*n)++]=b;out[(*n)++]=c;}

int nv2a_metal_draw(const NV2ATextureCopy*s,const uint8_t*texture,size_t texture_size,
 uint8_t*target,size_t target_size,uint8_t*depth,size_t depth_size,
 const float(*vertices)[16][4],unsigned count,unsigned primitive)
{
 if(!s)return reject("null-state");
 if((s->texture_mask&1)&&!texture)return reject("missing-texture");
 if(!target)return reject("missing-target");
 if(!vertices||count<3||count>4096)return reject("vertex-count");
 if(s->target_bpp!=2)return reject("target-format");
 if(!s->clip_w||!s->clip_h||s->clip_x||s->clip_y||s->clip_w>4096||s->clip_h>4096)return reject("clip");
 if(s->target_pitch<(uint64_t)s->clip_w*2)return reject("target-pitch");
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){
    if(u&&!s->extra_stages)return reject("missing-stage-state");
    const NV2ATextureCopy*t=u?&s->extra_stages[u-1]:s;
    const uint8_t*data=u?s->extra_texture[u-1]:texture;
    size_t bytes=u?s->extra_size[u-1]:texture_size;
    if(!data)return reject("missing-texture");
    if(!t->width||!t->height||t->width>4096||t->height>4096||!t->levels||t->levels>13)return reject("texture-size");
    if(nv2a_texture_copy_texture_bytes(t)>bytes)return reject("texture-bounds");
 }
 if((uint64_t)s->target_pitch*s->clip_h>target_size)return reject("target-bounds");
 if((s->depth_test||s->stencil_test)&&(!depth||s->depth_pitch<(uint64_t)s->clip_w*4||(uint64_t)s->depth_pitch*s->clip_h>depth_size))return reject("depth-bounds");
 unsigned indices[12288],n=0;
 switch(primitive){case 5:for(unsigned i=0;i+2<count;i+=3)triangle(s,vertices,indices,&n,i,i+1,i+2);break;
 case 6:for(unsigned i=0;i+2<count;i++)triangle(s,vertices,indices,&n,i+(i&1),i+1-(i&1),i+2);break;
 case 7:for(unsigned i=1;i+1<count;i++)triangle(s,vertices,indices,&n,0,i,i+1);break;
 case 8:for(unsigned i=0;i+3<count;i+=4){triangle(s,vertices,indices,&n,i,i+1,i+2);triangle(s,vertices,indices,&n,i,i+2,i+3);}break;
 case 9:for(unsigned i=0;i+3<count;i+=2){triangle(s,vertices,indices,&n,i,i+1,i+3);triangle(s,vertices,indices,&n,i,i+3,i+2);}break;default:return reject("primitive");}
 if(!n){reject_reason=NULL;return 0;}
 @autoreleasepool{if(!initialize())return reject("initialization");
  int use_zeta=s->depth_test||s->stencil_test;uint8_t*next_depth=use_zeta?depth:NULL;uint32_t next_depth_pitch=use_zeta?s->depth_pitch:0;size_t next_depth_size=use_zeta?depth_size:0;
  Vertex small_vertices[36];size_t vertex_bytes=count*sizeof(Vertex);id<MTLBuffer>vb=nil;
  Vertex*v;if(vertex_bytes<=sizeof(small_vertices)){v=small_vertices;++inline_vertex_batches;}else{vb=[device newBufferWithLength:vertex_bytes options:MTLResourceStorageModeShared];if(!vb)return reject("buffer-allocation");v=vb.contents;++allocated_vertex_batches;}
  size_t index_bytes=n*sizeof(indices[0]);id<MTLBuffer>ib=nil;if(index_bytes>4096){ib=[device newBufferWithBytes:indices length:index_bytes options:MTLResourceStorageModeShared];if(!ib)return reject("buffer-allocation");}
  id<MTLBuffer>tb[4];
  for(unsigned u=0;u<4;u++){
   int active=(s->texture_mask&(1u<<u))!=0;const NV2ATextureCopy*t=u&&active?&s->extra_stages[u-1]:s;
   const uint8_t*data=active?(u?s->extra_texture[u-1]:texture):NULL;size_t bytes=active?nv2a_texture_copy_texture_bytes(t):0;
   tb[u]=texture_buffer(data,bytes);if(!tb[u])return reject("buffer-allocation");}
  for(unsigned i=0;i<count;i++){memcpy(v[i].p,vertices[i][0],16);memcpy(v[i].d0,vertices[i][3],16);memcpy(v[i].d1,vertices[i][4],16);for(unsigned u=0;u<4;u++)memcpy(v[i].t[u],vertices[i][9+u],16);}
  if(!surface_valid||!depth_valid||surface_target!=target||surface_width!=s->clip_w||surface_height!=s->clip_h||surface_pitch!=s->target_pitch||surface_target_size!=target_size||depth_target!=next_depth||depth_pitch!=next_depth_pitch||depth_target_size!=next_depth_size){
   if(!nv2a_metal_sync())return reject("surface-sync");MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:s->clip_w height:s->clip_h mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;surface=[device newTextureWithDescriptor:td];MTLTextureDescriptor*sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:s->clip_w height:s->clip_h mipmapped:NO];sd.usage=MTLTextureUsageRenderTarget;sd.storageMode=MTLStorageModeShared;stencil_surface=[device newTextureWithDescriptor:sd];if(!surface||!stencil_surface)return reject("surface-allocation");
   surface_target=target;surface_target_size=target_size;surface_width=s->clip_w;surface_height=s->clip_h;surface_pitch=s->target_pitch;size_t pixels=(size_t)surface_width*surface_height;float*rgba=malloc(pixels*16);uint8_t*stencil=malloc(pixels);if(!rgba||!stencil){free(rgba);free(stencil);return reject("upload-allocation");}
   for(unsigned y=0;y<surface_height;y++)for(unsigned x=0;x<surface_width;x++){const uint8_t*p=target+(size_t)y*surface_pitch+x*2;unsigned c=p[0]|(unsigned)p[1]<<8;size_t at=((size_t)y*surface_width+x)*4;rgba[at]=(float)(c>>11)/31;rgba[at+1]=(float)((c>>5)&63)/63;rgba[at+2]=(float)(c&31)/31;if(next_depth){const uint8_t*z=next_depth+(size_t)y*next_depth_pitch+x*4;uint32_t q=(uint32_t)z[1]|(uint32_t)z[2]<<8|(uint32_t)z[3]<<16;rgba[at+3]=(float)q/16777215;stencil[(size_t)y*surface_width+x]=z[0];}else{rgba[at+3]=1;stencil[(size_t)y*surface_width+x]=0;}}
   [surface replaceRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0 withBytes:rgba bytesPerRow:surface_width*16];[stencil_surface replaceRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0 withBytes:stencil bytesPerRow:surface_width];free(rgba);free(stencil);surface_valid=depth_valid=1;surface_dirty=depth_dirty=0;depth_target=next_depth;depth_target_size=next_depth_size;depth_pitch=next_depth_pitch;}
  MTLRenderPassDescriptor*pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=surface;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[1].texture=stencil_surface;pass.colorAttachments[1].loadAction=MTLLoadActionLoad;pass.colorAttachments[1].storeAction=MTLStoreActionStore;
  id<MTLCommandBuffer>command=[queue commandBuffer];id<MTLRenderCommandEncoder>encoder=[command renderCommandEncoderWithDescriptor:pass];if(!command||!encoder)return reject("command-encoder");
  Params p={0};p.width=s->clip_w;p.height=s->clip_h;p.dither=s->dither;p.untextured=s->untextured;p.combiner_count=s->combiner_count;p.texture_mask=s->texture_mask;p.add_specular=s->add_specular;p.alpha_test=s->alpha_test;p.alpha_ref=s->alpha_ref;p.modulate=s->modulate;p.blend=s->blend;p.blend_src=s->blend_src;p.blend_dst=s->blend_dst;p.depth_test=s->depth_test;p.depth_write=s->depth_test&&s->depth_write;p.depth_func=s->depth_func;p.stencil_test=s->stencil_test;p.stencil_write=s->stencil_write;p.stencil_mask=s->stencil_mask;p.stencil_ref=s->stencil_ref;p.stencil_func_mask=s->stencil_func_mask;p.stencil_func=s->stencil_func;p.stencil_fail=s->stencil_fail;p.stencil_zfail=s->stencil_zfail;p.stencil_zpass=s->stencil_zpass;for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){const NV2ATextureCopy*t=u?&s->extra_stages[u-1]:s;p.tw[u]=t->width;p.th[u]=t->height;p.pitch[u]=t->pitch;p.linear[u]=t->linear;p.rgba8[u]=t->rgba8;p.dxt1[u]=t->dxt1;p.dxt3[u]=t->dxt3;p.repeat[u]=t->repeat;p.levels[u]=t->levels;p.min_filter[u]=t->min_filter;p.lod_bias[u]=t->lod_bias;}memcpy(p.color_icw,s->color_icw,sizeof(p.color_icw));memcpy(p.alpha_icw,s->alpha_icw,sizeof(p.alpha_icw));memcpy(p.color_ocw,s->color_ocw,sizeof(p.color_ocw));memcpy(p.alpha_ocw,s->alpha_ocw,sizeof(p.alpha_ocw));
  /* Do not let the hardware clip in Z.
   *
   * This path does its own depth test in the fragment shader, against a 24-bit
   * Z packed into the surface's alpha channel -- there is no depth attachment
   * and no hardware depth state. Metal still clips to the standard volume
   * 0 <= z <= w by default, so any triangle whose interpolated z leaves [0,1]
   * is thrown away BEFORE the shader can judge it, and whole flat pieces of
   * background vanish as the camera moves. Clamp instead: the shader owns the
   * decision, and geometry outside the range is depth-tested rather than
   * discarded.
   *
   * OPT-IN, because clamping is a DIAGNOSTIC here and not the fix. Measured
   * A/B in one binary: it recovers about 20% more triangles -- 18.46M against
   * 15.26M, which proves geometry really is being z-clipped -- but it halves
   * the frame rate, 6085 controller polls against 11980 over the same window.
   * Geometry past the far plane is squashed onto it and rasterised across
   * large areas, which is both expensive and wrong.
   *
   * So the clipping is a SYMPTOM. Something upstream is producing z outside
   * [0,1] for geometry the title expects to be visible, and that is what needs
   * fixing. Turn this on to confirm a suspected clip, not to ship. */
  {static int clamp_z=-1;if(clamp_z<0)clamp_z=getenv("RECOMP_METAL_DEPTH_CLAMP")?1:0;
   if(clamp_z)[encoder setDepthClipMode:MTLDepthClipModeClamp];}
  [encoder setRenderPipelineState:pipeline];if(vb)[encoder setVertexBuffer:vb offset:0 atIndex:0];else[encoder setVertexBytes:v length:vertex_bytes atIndex:0];[encoder setVertexBytes:&p length:sizeof(p) atIndex:1];if(ib)[encoder setVertexBuffer:ib offset:0 atIndex:2];else[encoder setVertexBytes:indices length:index_bytes atIndex:2];[encoder setFragmentBuffer:tb[0] offset:0 atIndex:0];[encoder setFragmentBuffer:tb[1] offset:0 atIndex:2];[encoder setFragmentBuffer:tb[2] offset:0 atIndex:3];[encoder setFragmentBuffer:tb[3] offset:0 atIndex:4];[encoder setFragmentBytes:&p length:sizeof(p) atIndex:1];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:n];[encoder endEncoding];[command commit];last_command=command;surface_dirty=1;if((s->depth_test&&s->depth_write)||(s->stencil_test&&s->stencil_write))depth_dirty=1;reject_reason=NULL;return(int)(n/3);}
}
