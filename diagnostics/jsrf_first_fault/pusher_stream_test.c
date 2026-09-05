#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned seen;
static uint32_t method[16],value[16];
int pgraph_d3d11_method(int subch,uint32_t m,uint32_t p) {
    CHECK(subch==0); CHECK(seen<16); method[seen]=m; value[seen++]=p; return 1;
}
void nv2a_pb_exec_method(uint32_t s,uint32_t m,uint32_t p) { (void)s; (void)m; (void)p; }
static unsigned software_seen;
static void software_method(uint32_t s, uint32_t p) {
    CHECK(s==0);
    CHECK(p==5 || p==9);
    CHECK(seen==6+software_seen); /* zero NOP precedes the first callback */
    ++software_seen;
}
int main(void) {
    const uint32_t packet[]={2u<<18|0x200,0x111,0x222,0x40000000u|2u<<18|0x1818,0x333,0x444};
    NV2APusherResult r=nv2a_pusher_run_segment(packet,2);
    CHECK(r.stop==NV2A_PUSHER_PARTIAL && r.consumed==0 && seen==0);
    r=nv2a_pusher_run_segment(packet,4);
    CHECK(r.stop==NV2A_PUSHER_PARTIAL && r.consumed==3 && r.methods==2 && seen==2);
    r=nv2a_pusher_run_segment(packet+3,3);
    CHECK(r.stop==NV2A_PUSHER_END && r.consumed==3 && r.methods==2 && seen==4);
    CHECK(method[0]==0x200 && method[1]==0x204 && method[2]==0x1818 && method[3]==0x1818);
    CHECK(value[0]==0x111 && value[1]==0x222 && value[2]==0x333 && value[3]==0x444);
    /* Ring tails can contain stale, valid-looking packets. Never consume them. */
    uint32_t tail[]={0x010dd001,1u<<18|0x208,0xdeadbeef};
    r=nv2a_pusher_run_segment(tail,3);
    CHECK(r.stop==NV2A_PUSHER_JUMP && r.jump_address==0x010dd000 && r.consumed==1 && seen==4);
    tail[0]=0x210dd000;
    r=nv2a_pusher_run_segment(tail,3);
    CHECK(r.stop==NV2A_PUSHER_JUMP && r.jump_address==0x010dd000 && seen==4);
    /* PFIFO's subroutine. Both opcodes stop the parse and hand the caller the
     * information it needs to keep the one-deep stack itself: CALL reports its
     * target, RETURN reports nothing but the fact. Each consumes exactly its
     * own dword, so the cursor the caller is left holding is the one hardware
     * saves as DMA_GET -- the dword after the call word. */
    tail[0]=0x028202A6;                 /* call 0x028202A4 */
    r=nv2a_pusher_run_segment(tail,3);
    CHECK(r.stop==NV2A_PUSHER_CALL && r.jump_address==0x028202A4
          && r.consumed==1 && seen==4);
    tail[0]=0x00020000;                 /* return */
    r=nv2a_pusher_run_segment(tail,3);
    CHECK(r.stop==NV2A_PUSHER_RETURN && r.consumed==1 && seen==4);
    /* A return must not be mistaken for a method packet: 0x00020000 masks to
     * neither header form, which is what used to make it read as malformed. */
    CHECK((0x00020000u & 0xE0030003u) != 0u
          && (0x00020000u & 0xE0030003u) != 0x40000000u);
    /* Data that decodes as neither is still refused. */
    tail[0]=0x00030000;
    r=nv2a_pusher_run_segment(tail,3);
    CHECK(r.stop==NV2A_PUSHER_INVALID && r.consumed==0 && seen==4);
    const uint32_t zero_count[]={0x200,0,1u<<18|0x208,0x113};
    r=nv2a_pusher_run_segment(zero_count,4);
    CHECK(r.stop==NV2A_PUSHER_END && r.consumed==4 && seen==5 && value[4]==0x113);
    nv2a_pusher_set_software_method_handler(software_method);
    const uint32_t notify[]={0x400c0100,0,5,9,0x40208,0x123};
    r=nv2a_pusher_run_segment(notify,6);
    CHECK(r.stop==NV2A_PUSHER_END && r.consumed==6 && software_seen==2);
    CHECK(seen==9 && method[8]==0x208 && value[8]==0x123);
    nv2a_pusher_set_software_method_handler(NULL);
    puts("Partial packets, both jump encodings, stale tails, CALL/RETURN and malformed headers passed");
}
