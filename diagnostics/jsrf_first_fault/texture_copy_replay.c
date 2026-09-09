/* Input is packed by replay_texture_copy.py, never a dumped host C struct. */
#include "nv2a_texture_copy.h"
#ifdef __APPLE__
#include "nv2a_metal.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void read_exact(FILE *f,void *p,size_t n) {
    if(fread(p,1,n,f)!=n) { fputs("truncated replay\n",stderr); exit(1); }
}
static uint32_t word(FILE *f) {
    uint8_t b[4]; read_exact(f,b,4);
    return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
int main(int argc,char **argv) {
    if(argc!=3 && argc!=4) { fputs("usage: jsrf_texture_copy_replay input output [depth-output]\n",stderr); return 1; }
    FILE *f=fopen(argv[1],"rb"); if(!f) { perror(argv[1]); return 1; }
    if(word(f)!=0x4354584e) return 1;
    uint32_t version=word(f);
    if((version!=1 && version!=2) || word(f)!=5) { fputs("unsupported replay\n",stderr); return 1; }
    uint32_t n=word(f),ts=word(f),ds=word(f),m[2048];
    uint32_t zs=version==2 ? word(f) : 0;
    if(n<3 || n>4096 || n%3 || !ts || !ds || ts>0x4000000 || ds>0x4000000 || zs>0x4000000) return 1;
    for(unsigned i=0;i<2048;++i) m[i]=word(f);
    float (*v)[16][4]=malloc((size_t)n*sizeof(*v));
    uint8_t *texture=malloc(ts),*target=malloc(ds);
    uint8_t *depth=zs ? malloc(zs) : NULL;
    if(!v || !texture || !target || (zs && !depth)) return 1;
    for(unsigned i=0;i<n;++i) for(int o=0;o<16;++o) for(int k=0;k<4;++k) {
        uint32_t bits=word(f); memcpy(&v[i][o][k],&bits,4);
    }
    read_exact(f,texture,ts); read_exact(f,target,ds);
    if(zs) read_exact(f,depth,zs);
    if(fgetc(f)!=EOF) return 1;
    fclose(f);
    NV2ATextureCopy state;
    const char *error=nv2a_texture_copy_prepare(m,&state);
    if(error) { fprintf(stderr,"unsupported state: %s\n",error); return 1; }
#ifdef __APPLE__
    if(getenv("RECOMP_METAL_REPLAY")) {
        if(nv2a_metal_draw(&state,texture,ts,target,ds,depth,zs,v,n,5)<0 || !nv2a_metal_sync()) {
            fputs("invalid Metal draw\n",stderr); return 1;
        }
    } else
#endif
    for(unsigned i=0;i<n;i+=3)
        if(!nv2a_texture_copy_triangle_depth(&state,texture,ts,target,ds,depth,zs,v[i],v[i+1],v[i+2])) {
            fputs("invalid triangle\n",stderr); return 1;
        }
    f=fopen(argv[2],"wb"); if(!f) return 1;
    if(fwrite(target,1,ds,f)!=ds || fclose(f)) return 1;
    if(argc==4) {
        f=fopen(argv[3],"wb"); if(!f) return 1;
        if((zs && fwrite(depth,1,zs,f)!=zs) || fclose(f)) return 1;
    }
    free(v); free(texture); free(target); free(depth);
    return 0;
}
