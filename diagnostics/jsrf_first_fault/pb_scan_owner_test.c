#define _POSIX_C_SOURCE 200809L
#include "nv2a_pb_scan.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t words[]={0x00040300u,1};
static unsigned executed,reported;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)words; }
void nv2a_pb_exec_method(uint32_t subch,uint32_t method,uint32_t param)
{
    CHECK(subch==0 && method==0x300 && param==1);
    ++executed;
}
void nv2a_pb_exec_report(void) { ++reported; }

int main(void)
{
    setenv("RECOMP_PB_EXEC","1",1);
    setenv("RECOMP_PB_SCAN","1",1);
    nv2a_pb_scan_set_external_executor(1);
    nv2a_pb_scan(0,sizeof(words));
    nv2a_pb_scan_report();
    CHECK(executed==0 && reported==0); /* survey must not touch executor state */
    nv2a_pb_scan_set_external_executor(0);
    nv2a_pb_scan(0,sizeof(words));
    nv2a_pb_scan_report();
    CHECK(executed==1 && reported==1); /* legacy standalone mode preserved */
    puts("Pushbuffer scan/execution ownership checks passed");
}
