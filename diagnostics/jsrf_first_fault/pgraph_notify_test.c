/* Software-method register handshake through the real AArch64 W1C trap. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xbox_memory_layout.h"
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void)
{
#if !defined(_WIN32) && defined(__aarch64__)
    uint8_t xbe[0x400] = {0};
    memcpy(xbe,"XBEH",4);
    *(uint32_t *)(xbe+0x104)=0x10000;
    *(uint32_t *)(xbe+0x108)=sizeof(xbe);
    *(uint32_t *)(xbe+0x120)=0x10000;
    CHECK(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
    volatile uint32_t *r=(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset()+0xFD000000u);
    CHECK(!xbox_Nv2aRaiseSoftwareMethod(0,0));
    CHECK(!xbox_Nv2aSoftwareMethodPending());
    CHECK(xbox_Nv2aRaiseSoftwareMethod(3,5));
    CHECK(r[0x400704/4]==0x30100 && r[0x400708/4]==5);
    CHECK(r[0x400108/4]==1 && (r[0x400100/4]&0x100000));
    CHECK(r[0x100/4]&0x1000);
    Sleep(20); /* The idle-ack worker must not steal modeled interrupts. */
    CHECK(r[0x400100/4]&0x100000);
    CHECK(r[0x100/4]&0x1000);
    CHECK(!xbox_Nv2aRaiseSoftwareMethod(0,9));
    CHECK(r[0x400708/4]==5);
    r[0x400100/4]=0; /* W1C zero is not an acknowledgement. */
    CHECK(xbox_Nv2aSoftwareMethodPending());
    xbox_Nv2aRaiseVblank();
    r[0x400100/4]=0x100000;
    CHECK(!(r[0x400100/4]&0x100000) && !(r[0x100/4]&0x1000));
    CHECK(r[0x100/4]&0x1000000); /* Preserve another engine's summary. */
    CHECK(xbox_Nv2aSoftwareMethodPending()); /* FIFO still disabled. */
    r[0x400720/4]=1;
    CHECK(!xbox_Nv2aSoftwareMethodPending());
    CHECK(xbox_Nv2aRaiseSoftwareMethod(0,2));
    r[0x600100/4]=1;
    CHECK(r[0x100/4]&0x1000); /* Vblank ack preserves PGRAPH. */
    r[0x400100/4]=0x100000;
    r[0x400720/4]=1;
    CHECK(!xbox_Nv2aSoftwareMethodPending());
#endif
    puts("Software trap payload, W1C, FIFO handoff and interrupt isolation passed");
    return 0;
}
