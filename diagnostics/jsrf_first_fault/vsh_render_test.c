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
    puts("NV2A upload cursors, shader outputs, oversized clipping and framebuffer pixels passed");
}
