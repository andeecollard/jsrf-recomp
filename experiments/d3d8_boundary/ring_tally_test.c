#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "recomp_mem_watch.h"
#include "d3d8_ring.h"
static uint32_t RLO, RHI;
int d3d8_ring_read_target(D3D8RingTarget *t){ memset(t,0,sizeof *t); t->device=0x19B200;
  t->ring_lo=RLO; t->ring_hi=RHI; t->put=RLO+16; t->trusted=1; return 1; }
#define SPAN (64u<<20)
static int check(const char *what, int cond){ printf("%s %s\n", cond?"PASS":"FAIL", what); return !cond; }
int main(void){
  static uint32_t cell; int bad=0;
  setenv("RECOMP_MEM_WATCH_TALLY","ring",1);
  /* Case 1: ring in the low window, mirror 0 at 64 MB. */
  RLO=0x00100000u; RHI=0x00180000u;
  recomp_mem_watch_init(SPAN, 1u, 0, 0);
  bad|=check("enabled", g_recomp_mem_watch_enabled==1);
  for(int i=0;i<5000;i++) recomp_mem_watch_guest_store(0x199301,0x199300,RLO+4*(i%100),4,&cell,i);
  for(int i=0;i<7;i++) recomp_mem_watch_guest_store(0x14D201,0x14D200,SPAN+0x00120000u,4,&cell,i); /* mirror of the ring */
  for(int i=0;i<9;i++) recomp_mem_watch_guest_store(0x11111,0x11110,0x00200000u,4,&cell,1000+i);    /* outside */
  bad|=check("store still performed", cell==1008);
  recomp_mem_watch_guest_block(0x199060,0x00140000u,64);
  recomp_mem_watch_guest_block(0x22220,0x00300000u,64);                                            /* block outside */
  recomp_mem_watch_tally_report("case1");
  /* Case 2: ring outside the alias model (raw compare only). */
  recomp_mem_watch_shutdown(); RLO=0xF0100000u; RHI=0xF0180000u;
  recomp_mem_watch_init(SPAN, 1u, 0, 0);
  for(int i=0;i<4100;i++) recomp_mem_watch_guest_store(0x199301,0x199399,RLO+8,4,&cell,i);
  recomp_mem_watch_tally_report("case2");
  recomp_mem_watch_shutdown();
  return bad;
}
