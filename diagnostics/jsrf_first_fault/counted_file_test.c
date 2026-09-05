/* Drive real guest ABI thunks, not a replacement path conversion. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va);
int xbox_VideoIsPlaying(void) { return 0; }
extern RECOMP_TLS uint32_t g_esp,g_eax;
static uint8_t xbe[0x400];
static uint8_t *ram;
#define W(a) (*(uint32_t *)(ram+(a)))
#define H(a) (*(uint16_t *)(ram+(a)))
#define TEST_APC_VA 0x00230000u
static unsigned apc_calls;
static uint32_t apc_context_seen, apc_iostatus_seen;
static void test_file_apc(void)
{
    apc_context_seen = W(g_esp + 4);
    apc_iostatus_seen = W(g_esp + 8);
    CHECK(W(g_esp + 12) == 0);
    W(apc_context_seen) = 0;
    ++apc_calls;
    g_esp += 16; /* ret 12 */
}
recomp_func_t recomp_lookup_manual(uint32_t va)
{
    return va == TEST_APC_VA ? test_file_apc : NULL;
}
static uint32_t call(unsigned slot,const uint32_t *args,unsigned n)
{
    g_esp=0x40000; W(g_esp)=0x12345;
    memcpy(ram+g_esp+4,args,n*4);
    recomp_func_t fn=recomp_lookup_kernel(W(0x20000+slot*4));
    CHECK(fn); fn();
    CHECK(g_esp==0x40000+4+n*4); /* includes the directory call's tenth argument */
    return g_eax;
}
int main(void)
{
    memcpy(xbe,"XBEH",4);
    *(uint32_t *)(xbe+0x104)=0x10000;
    *(uint32_t *)(xbe+0x108)=sizeof(xbe);
    *(uint32_t *)(xbe+0x120)=0x10000;
    CHECK(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
    ram=(uint8_t *)xbox_GetMemoryOffset();
    const unsigned ord[]={190,202,207,187,210,219,99};
    for(unsigned i=0;i<7;++i) W(0x20000+i*4)=0x80000000u|ord[i];
    xbox_kernel_set_thunk_address(0x20000,7); xbox_kernel_bridge_init();
    char temp[]="/tmp/jsrf-counted-file-XXXXXX";
    CHECK(mkdtemp(temp)); xbox_path_init(temp,temp);
    char cache[256],media[256],dir[256],file[256];
    snprintf(cache,sizeof(cache),"%s/Cache",temp); CHECK(!mkdir(cache,0700));
    snprintf(media,sizeof(media),"%s/Cache/Media",temp); CHECK(!mkdir(media,0700));
    snprintf(dir,sizeof(dir),"%s/Cache/Media/Cache",temp); CHECK(!mkdir(dir,0700));
    const char *marker="JSRF_CACHE_COMPLETE00.CMP", *parent="Z:\\Media\\Cache\\";
    snprintf(file,sizeof(file),"%s/%s",dir,marker);
    FILE *f=fopen(file,"wb"); CHECK(f); CHECK(fwrite("ADX",1,3,f)==3); CHECK(!fclose(f));
    unsigned plen=(unsigned)strlen(parent), mlen=(unsigned)strlen(marker);
    memcpy(ram+0x31000,parent,plen); memcpy(ram+0x31000+plen,marker,mlen);
    memcpy(ram+0x31000+plen+mlen,"NOT_PART_OF_NAME",17);
    W(0x30000)=0xfffffffdu; W(0x30004)=0x30020; W(0x30008)=0x40;
    H(0x30020)=H(0x30022)=plen; W(0x30024)=0x31000;
    H(0x30070)=H(0x30072)=mlen; W(0x30074)=0x31000+plen;
    uint32_t open_args[]={0x30040,0x100001,0x30000,0x30050,3,0x4021};
    for(unsigned cycle=0;cycle<96;++cycle) {
        CHECK(call(1,open_args,6)==0);
        uint32_t token=W(0x30040);
        uint32_t query[]={token,0,0,0,0x30050,0x32000,0x148,1,0x30070,0};
        memset(ram+0x31ff0,0xa5,0x168);
        CHECK(call(2,query,10)==0);
        CHECK(W(0x30050)==0 && W(0x30054)==64+mlen);
        CHECK(W(0x32000+60)==mlen && !memcmp(ram+0x32000+64,marker,mlen));
        for(int i=0;i<16;++i) CHECK(ram[0x31ff0+i]==0xa5 && ram[0x32148+i]==0xa5);
        if(cycle==0) {
            CHECK(call(2,query,10)==(uint32_t)STATUS_NO_MORE_FILES);
            query[9]=1; CHECK(call(2,query,10)==0); /* RestartScan is arg 9 */
            query[6]=8; memset(ram+0x32000,0xa5,0x148);
            CHECK(call(2,query,10)==(uint32_t)STATUS_BUFFER_TOO_SMALL);
            for(unsigned i=0;i<0x148;++i) CHECK(ram[0x32000+i]==0xa5);
            query[6]=0x148; query[7]=99;
            CHECK(call(2,query,10)==(uint32_t)STATUS_INVALID_INFO_CLASS);
        }
        CHECK(call(3,&token,1)==0); /* releases enumeration context before EOF */
    }
    /* A complete counted path need not have a NUL either. */
    H(0x30020)=H(0x30022)=plen+mlen;
    uint32_t attrs[]={0x30000,0x32000};
    CHECK(call(4,attrs,2)==0);
    CHECK(!memcmp(ram+0x31000,parent,plen)); /* no inserted guest terminator */
    CHECK(!memcmp(ram+0x31000+plen,marker,mlen));
    /* Synchronous host I/O still has asynchronous guest APC ordering. The
     * callback must remain queued after NtReadFile returns, then run on the
     * issuing thread's next alertable wait and leave that wait's stack exact. */
    uint32_t file_open_args[]={0x30040,0x100001,0x30000,0x30050,3,0x4020};
    CHECK(call(1,file_open_args,6)==0);
    uint32_t token=W(0x30040);
    W(0x33020)=W(0x33024)=0; W(0x33040)=1;
    uint32_t read_args[]={token,0,TEST_APC_VA,0x33040,0x33000,0x34000,3,0x33020};
    CHECK(call(5,read_args,8)==0);
    CHECK(apc_calls==0 && W(0x33040)==1);
    CHECK(W(0x33000)==0 && W(0x33004)==3);
    CHECK(!memcmp(ram+0x34000,"ADX",3));
    uint32_t alertable_delay[]={0,1,0};
    CHECK(call(6,alertable_delay,3)==0xC0);
    CHECK(apc_calls==1 && apc_context_seen==0x33040);
    CHECK(apc_iostatus_seen==0x33000 && W(0x33040)==0);
    CHECK(call(3,&token,1)==0);
    /* Remove only this test's newly created fixture/directories. */
    CHECK(!unlink(file)); CHECK(!rmdir(dir)); CHECK(!rmdir(media)); CHECK(!rmdir(cache)); CHECK(!rmdir(temp));
    xbox_MemoryLayoutShutdown();
    puts("Counted paths, directory ABI, deferred APC, guest stack, output bounds and context release passed");
}
