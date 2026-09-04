/* Drive the public pushbuffer method sink and inspect real framebuffer pixels. */
#include "nv2a_regs.h"
#include "vsh_capture.h"
#include "texture_copy_state.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t ram[16384];
static uint8_t gpu_regs[0x800000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
void *xbox_GpuMemoryRange(uint32_t address, size_t bytes) {
    return (uint64_t)address+bytes<=sizeof(ram) ? (uint8_t *)ram+address : NULL;
}
const uint8_t *xbox_Nv2aRegisterMemory(void) { return gpu_regs; }
/* The renderer asks the guest heap who owns a vertex array when it samples a
 * draw. There is no heap in this harness, and the answer does not affect what
 * is rendered. */
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size)
{
    (void)xbox_va;
    if (buf && size) snprintf(buf, size, "no heap in this harness");
    return 0;
}
void xbox_FramebufferWindowSet(uint32_t a, uint32_t p) { (void)a; (void)p; }
void xbox_FramebufferWindowStart(void) {}
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_report(void);
int nv2a_pb_exec_vsh_constant(unsigned index, float out[4]);
static void put(uint32_t m,uint32_t p) { nv2a_pb_exec_method(0,m,p); }
static void fp(uint32_t m,float f) { uint32_t u; memcpy(&u,&f,4); put(m,u); }
static uint32_t *pixels(void) { return ram+0x1000/4; }
static void draw(void) {
    put(NV097_SET_BEGIN_END,NV097_SET_BEGIN_END_OP_TRIANGLES);
    const float v[3][4]={{-0.53125f,-0.53125f,0,0},{2559.46875f,-0.53125f,2560,0},{-0.53125f,1919.46875f,0,1920}};
    for(int i=0;i<3;++i) for(int k=0;k<4;++k) fp(NV097_INLINE_ARRAY,v[i][k]);
    put(NV097_SET_BEGIN_END,0);
}
int main(void) {
    put(NV097_SET_SURFACE_CLIP_HORIZONTAL,8u<<16);
    put(NV097_SET_SURFACE_CLIP_VERTICAL,8u<<16);
    put(NV097_SET_SURFACE_PITCH,32);
    put(NV097_SET_SURFACE_COLOR_OFFSET,0x1000);
    put(NV097_SET_COLOR_CLEAR_VALUE,0);
    put(NV097_CLEAR_SURFACE,0xF0);
    put(NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x22); /* float2 v0 */
    put(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+9*4,0x22); /* float2 v9 */
    put(NV097_SET_VERTEX_DATA4UB+3*4,0xFF00FF00); /* current v3 = green */
    put(NV097_SET_TRANSFORM_EXECUTION_MODE,2);
    put(NV097_SET_TRANSFORM_PROGRAM_LOAD,4);
    /* Upload >8 slots: method window wraps, LOAD must keep advancing. */
    for(unsigned i=0;i<sizeof(jsrf_vsh_words)/4;++i)
        put(NV097_SET_TRANSFORM_PROGRAM+(i%32)*4,jsrf_vsh_words[i]);
    put(NV097_SET_TRANSFORM_PROGRAM_START,4);
    put(NV097_SET_TRANSFORM_CONSTANT_LOAD,0);
    const float c[]={1,1,16777215,1,0.53125f,0.53125f,0,0};
    for(int i=0;i<8;++i) fp(NV097_SET_TRANSFORM_CONSTANT+i*4,c[i]);
    draw();
    for(int i=0;i<64;++i) CHECK(pixels()[i]==0xFF00FF00);
    /* Update constants without re-uploading code, moving the left edge to x=4. */
    put(NV097_CLEAR_SURFACE,0xF0);
    put(NV097_SET_TRANSFORM_CONSTANT_LOAD,1);
    fp(NV097_SET_TRANSFORM_CONSTANT,4.53125f);
    draw();
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        CHECK(pixels()[y*8+x] == (x<4 ? 0 : 0xFF00FF00));
    /* START changes invalidate the decoded program; uninitialised slots cannot draw. */
    put(NV097_CLEAR_SURFACE,0xF0);
    put(NV097_SET_TRANSFORM_PROGRAM_START,100);
    draw();
    for(int i=0;i<64;++i) CHECK(pixels()[i]==0);
    /* Send the real fragment state through the public method sink. Both DMA
     * handles deliberately use a nonzero base, and source rows are padded. */
    uint32_t m[2048];
    copy_methods(m,8,8,20,16,2);
    copy_dma(gpu_regs+0x700000,0x2000,0xffff);
    for(unsigned a=0;a<2048;++a) if(m[a]) put(a*4,m[a]);
    put(NV097_SET_TRANSFORM_PROGRAM_START,4);
    put(NV097_SET_TRANSFORM_CONSTANT_LOAD,1);
    fp(NV097_SET_TRANSFORM_CONSTANT,.53125f);
    uint8_t *source=(uint8_t *)ram+0x3000, *destination=(uint8_t *)ram+0x4000;
    for(int y=0;y<8;++y) for(int x=0;x<20;++x) source[y*20+x]=(uint8_t)(x*17+y*23);
    memset(destination,0xaa,128);
    draw();
    for(int y=0;y<8;++y) CHECK(!memcmp(source+y*20,destination+y*16,16));
    put(NV097_SET_BLEND_ENABLE,1); /* Explicit rejection must not paint white. */
    memset(destination,0x55,128); draw();
    for(int i=0;i<128;++i) CHECK(destination[i]==0x55);
    put(NV097_SET_BLEND_ENABLE,0);
    modulate_methods(m);
    for(unsigned a=0;a<2048;++a) if(m[a]) put(a*4,m[a]);
    /* Current diffuse is green: modulated RGB565 keeps only source green. */
    draw();
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        unsigned src=source[y*20+x*2]|(unsigned)source[y*20+x*2+1]<<8;
        unsigned dst=destination[y*16+x*2]|(unsigned)destination[y*16+x*2+1]<<8;
        CHECK(dst==(src&0x07e0));
    }
    /* Alternate strip winding must keep both front-facing triangles. */
    put(NV097_SET_CULL_FACE_ENABLE,1);
    put(NV097_SET_CULL_FACE,0x405); put(NV097_SET_FRONT_FACE,0x900);
    memset(source,0xff,160); memset(destination,0,128);
    put(NV097_SET_BEGIN_END,NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP);
    for(int i=0;i<4;++i) {
        fp(NV097_INLINE_ARRAY,(i&1) ? 7.46875f : -.53125f);
        fp(NV097_INLINE_ARRAY,(i&2) ? 7.46875f : -.53125f);
        fp(NV097_INLINE_ARRAY,.5f); fp(NV097_INLINE_ARRAY,.5f);
    }
    put(NV097_SET_BEGIN_END,0);
    for(int i=0;i<64;++i) CHECK(destination[i*2]==0xe0 && destination[i*2+1]==7);
    /* Depth-only clears resolve the DMA base and preserve the other plane.
     * Clear rectangle is inclusive, row padding and exterior stay untouched. */
    put(NV097_SET_SURFACE_FORMAT,0x123);
    put(NV097_SET_CONTEXT_DMA_ZETA,9);
    put(NV097_SET_SURFACE_ZETA_OFFSET,0x3000);
    put(NV097_SET_SURFACE_PITCH,40u<<16|16);
    put(NV097_SET_CLEAR_RECT_HORIZONTAL,3u<<16|1);
    put(NV097_SET_CLEAR_RECT_VERTICAL,2u<<16|1);
    put(NV097_SET_ZSTENCIL_CLEAR_VALUE,0x123456ab);
    uint8_t *depth=(uint8_t *)ram+0x5000;
    memset(depth,0x55,320);
    put(NV097_CLEAR_SURFACE,1);
    for(int y=0;y<8;++y) for(int x=0;x<10;++x) {
        uint32_t z; memcpy(&z,depth+y*40+x*4,4);
        CHECK(z==((y>=1 && y<=2 && x>=1 && x<=3) ? 0x12345655u : 0x55555555u));
    }
    put(NV097_CLEAR_SURFACE,2);
    uint32_t z; memcpy(&z,depth+40+4,4); CHECK(z==0x123456ab);
    /* Truncated DMA / unmapped address cannot mutate the original depth. */
    put(NV097_SET_SURFACE_ZETA_OFFSET,0xffff);
    put(NV097_SET_ZSTENCIL_CLEAR_VALUE,0xffffffff); put(NV097_CLEAR_SURFACE,3);
    memcpy(&z,depth+40+4,4); CHECK(z==0x123456ab);
    /* The viewport transform lives in the vertex constant file.
     *
     * NV2A keeps the viewport scale and offset at fixed constant slots -- 58
     * (NV_IGRAPH_XF_XFCTX_VPSCL) and 59 (VPOFF) -- and titles' vertex programs
     * read them there. JSRF's ends with
     *     mul oPos.xyz, R12, c58
     *     mad oPos.xyz, R12, R1, c59
     * so with those slots left at zero every vertex transformed to x=y=z=0
     * while w, written one instruction earlier and not in the xyz write mask,
     * stayed correct. 714,240 draws a run collapsed to a single point.
     *
     * The methods were being recorded for the rasteriser but never mirrored
     * into the constant file, which is what hardware does. */
    {
        float c[4];
        fp(NV097_SET_VIEWPORT_SCALE + 0,  320.0f);
        fp(NV097_SET_VIEWPORT_SCALE + 4, -240.0f);
        fp(NV097_SET_VIEWPORT_SCALE + 8,  16777215.0f);
        fp(NV097_SET_VIEWPORT_SCALE + 12,   0.0f);
        fp(NV097_SET_VIEWPORT_OFFSET + 0, 320.53125f);
        fp(NV097_SET_VIEWPORT_OFFSET + 4, 240.53125f);
        fp(NV097_SET_VIEWPORT_OFFSET + 8,   0.0f);
        fp(NV097_SET_VIEWPORT_OFFSET + 12,  0.0f);
        CHECK(nv2a_pb_exec_vsh_constant(0x3a, c));
        CHECK(c[0] == 320.0f && c[1] == -240.0f && c[2] == 16777215.0f);
        CHECK(nv2a_pb_exec_vsh_constant(0x3b, c));
        CHECK(c[0] == 320.53125f && c[1] == 240.53125f);
        /* A slot the title never wrote stays zero -- the mirror must not
         * scribble across the constant file. */
        CHECK(nv2a_pb_exec_vsh_constant(0x39, c));
        CHECK(c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f);
    }
    if (getenv("RECOMP_COMBINER_TRACE")) nv2a_pb_exec_report();
    puts("NV2A upload cursors, shader outputs, oversized clipping and framebuffer pixels passed");
}
