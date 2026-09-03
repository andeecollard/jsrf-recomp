#ifndef TEXTURE_COPY_STATE_H
#define TEXTURE_COPY_STATE_H
#include <stdint.h>
#include <string.h>
/* Measured JSRF copy program, with synthetic surface sizes/addresses. */
static void copy_methods(uint32_t m[2048], unsigned width, unsigned height,
                         unsigned texture_pitch, unsigned target_pitch, unsigned bpp)
{
    memset(m,0,2048*sizeof(*m));
#define M(a) m[(a)/4]
    M(0x184)=3; M(0x188)=3; M(0x194)=9;
    M(0x1b00)=0x1000; M(0x1b04)=0x11129; M(0x1b08)=0x10303;
    M(0x1b0c)=0x4003ffc0; M(0x1b10)=texture_pitch<<16;
    M(0x1b14)=0x02063f34; M(0x1b1c)=width<<16|height;
    M(0x1e70)=1; M(0x1e60)=1; M(0xac0)=0x08200000;
    M(0x260)=0x14200000; M(0xaa0)=0xc00; M(0x1e40)=0xc00;
    M(0x288)=0xc; M(0x28c)=0x1c80; M(0x358)=0x01010101;
    M(0x37c)=0x1d01; M(0x38c)=M(0x390)=0x1b02;
    M(0x208)=bpp==2 ? 0x113 : 0x118; M(0x210)=0x2000;
    M(0x20c)=target_pitch; M(0x200)=width<<16; M(0x204)=height<<16;
    M(0x2c0)=(width-1)<<16; M(0x2e0)=(height-1)<<16;
#undef M
}
static void le32(uint8_t *p,uint32_t v) { for(int k=0;k<4;++k) p[k]=(uint8_t)(v>>(8*k)); }
static void copy_dma(uint8_t *ramin, uint32_t base, uint32_t limit)
{
    le32(ramin+0x18,3); le32(ramin+0x1c,0x80000112);
    le32(ramin+0x48,9); le32(ramin+0x4c,0x80000112);
    le32(ramin+0x1120,0xb03d | (base&4095)<<20);
    le32(ramin+0x1124,limit); le32(ramin+0x1128,(base&~4095u)|3);
}
#endif
