/* Read-only, opt-in observations at real generated-code sites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
static uint32_t read_word(uint32_t address) {
    uint32_t value=0;
    const void *p=xbox_GpuMemoryRange(address,4);
    if(p) memcpy(&value,p,4);
    return value;
}
void jsrf_startup_probe(uint32_t pc,uint32_t object)
{
    static int enabled=-1;
    static unsigned ticks, opens, total;
    static uint32_t last_root[10];
    static struct { uint32_t object,pc,flags,target; } seen[256];
    static unsigned count;
    if(enabled<0) enabled=getenv("RECOMP_STARTUP_TRACE")!=NULL;
    if(!enabled) return;
    if(pc==0x13a80) {
        const unsigned offsets[]={0x24,0x40,0x44,0x48,0x4c,0x74,0x94,0x7f9c,0x87dc,0x87e8};
        uint32_t state[10];
        for(unsigned i=0;i<10;++i) state[i]=read_word(object+offsets[i]);
        ++ticks;
        if(ticks<=3 || memcmp(state,last_root,sizeof(state)) || ticks%10000==0) {
            fprintf(stderr,"[STARTUP] tick=%u root=%08X",ticks,object);
            for(unsigned i=0;i<10;++i) fprintf(stderr," +%04X=%08X",offsets[i],state[i]);
            fputc('\n',stderr); memcpy(last_root,state,sizeof(state));
        }
        return;
    }
    if(pc==0x25dd0) {
        if(++opens>32) return;
        char path[257]={0};
        for(unsigned i=0;i<256;++i) {
            const char *p=xbox_GpuMemoryRange(object+i,1);
            if(!p) break;
            path[i]=*p; if(!path[i]) break;
        }
        fprintf(stderr,"[STARTUP-ASSET] call=%u return=%08X pathptr=%08X path=%s\n",
                opens,read_word(g_esp),object,path);
        return;
    }
    if(!object || !xbox_GpuMemoryRange(object,128)) return;
    uint32_t flags=read_word(object+4), vtable=read_word(object);
    uint32_t target=read_word(vtable+(pc==0x11083 ? 4 : 0xc));
    ++total;
    for(unsigned i=0;i<count;++i)
        if(seen[i].object==object && seen[i].pc==pc && seen[i].flags==flags && seen[i].target==target) return;
    if(count==256) return;
    seen[count].object=object; seen[count].pc=pc; seen[count].flags=flags; seen[count++].target=target;
    fprintf(stderr,"[STARTUP-OBJECT] pc=%08X object=%08X vtable=%08X flags=%08X target=%08X calls=%u words=",
            pc,object,vtable,flags,target,total);
    for(unsigned i=0;i<32;++i) fprintf(stderr,"%s%08X",i ? "," : "",read_word(object+i*4));
    fputc('\n',stderr);
}
