/* Optional differential test. Supply JSRF_VSH_REFERENCE_DIR pointing at an
 * independent checkout of https://github.com/abaire/nv2a_vsh_cpu/src.
 * No reference implementation is linked into the runtime. */
#include "nv2a_vsh.h"
#include "nv2a_vsh_emulator.h"
#include "vsh_capture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void)
{
    NV2AVshProgram ours;
    Nv2aVshProgram reference;
    CHECK(nv2a_vsh_parse(jsrf_vsh_words,12,&ours));
    CHECK(nv2a_vsh_parse_program(&reference,jsrf_vsh_words,12)==NV2AVPR_SUCCESS);
    for(int trial=0;trial<128;++trial) {
        Nv2aVshCPUFullExecutionState storage;
        Nv2aVshExecutionState state=nv2a_vsh_emu_initialize_full_execution_state(&storage);
        float (*inputs)[4]=(float(*)[4])storage.input_regs;
        float (*constants)[4]=(float(*)[4])storage.context_regs;
        for(int i=0;i<16;++i) for(int k=0;k<4;++k)
            inputs[i][k]=((trial*7+i*13+k*5)%61-30)*0.125f;
        inputs[0][3]=0.25f+(trial%16)*0.125f;
        for(int i=0;i<192;++i) for(int k=0;k<4;++k)
            constants[i][k]=((trial*3+i*7+k*11)%53-26)*0.25f;
        NV2AVshResult result;
        CHECK(nv2a_vsh_execute(&ours,inputs,constants,&result));
        nv2a_vsh_emu_execute(&state,&reference);
        for(int o=0;o<13;++o) {
            if(o==1 || o==2) continue;
            for(int k=0;k<(o==5?1:4);++k) {
                float a=result.output[o][k], b=storage.output_regs[o*4+k];
                if (!(fabsf(a-b)<=0.0001f*fmaxf(1.0f,fabsf(b)))) {
                    fprintf(stderr,"trial=%d output=%d component=%d ours=%g reference=%g\n",trial,o,k,a,b);
                    return 1;
                }
            }
        }
    }
    nv2a_vsh_program_destroy(&reference);
    puts("Captured JSRF program: 128 varied input/constant sets match the independent interpreter");
}
