#include "nv2a_ff.h"
#include <math.h>
#include <string.h>
static float value(const uint32_t *m,unsigned byte)
{ float f;memcpy(&f,&m[byte/4],4);return f; }
static void matrix(const uint32_t *m,unsigned base,const float in[4],float out[4])
{
    for(unsigned row=0;row<4;++row) {
        out[row]=0;
        for(unsigned col=0;col<4;++col) out[row]+=value(m,base+row*16+col*4)*in[col];
    }
}
static float length3(const float v[4])
{ return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
const char *nv2a_ff_vertex(const uint32_t m[2048],const float in[16][4],float out[16][4])
{
    static const uint32_t normal_map=0x8511;
    if(m[0x314/4] || m[0x328/4]) return "fixed-function lighting / skinning";
    memset(out,0,16*4*sizeof(float));
    matrix(m,0x680,in[0],out[0]);
    if(!isfinite(out[0][3]) || out[0][3]==0) return "fixed-function clip W";
    for(unsigned k=0;k<3;++k) {
        out[0][k]/=out[0][3];
        if(k<2) out[0][k]+=value(m,0xa20+4*k);
        if(!isfinite(out[0][k])) return "fixed-function position";
    }
    memcpy(out[3],in[3],4*sizeof(float));
    memcpy(out[4],in[4],4*sizeof(float));
    float normal_in[4]={in[2][0],in[2][1],in[2][2],0},normal[4];
    matrix(m,0x580,normal_in,normal);
    if(m[0x3a4/4]) {
        float n=length3(normal);
        if(!isfinite(n) || n==0) return "fixed-function normal";
        for(unsigned k=0;k<3;++k) normal[k]/=n;
    }
    for(unsigned u=0;u<4;++u) {
        float generated[4];
        for(unsigned k=0;k<4;++k) {
            uint32_t mode=m[(0x3c0+u*16+k*4)/4];
            if(!mode) generated[k]=in[9+u][k];
            else if(mode==normal_map && k<3) generated[k]=normal[k];
            else return "fixed-function texgen";
            if(!isfinite(generated[k])) return "fixed-function texcoord";
        }
        if(m[(0x420+u*4)/4]) matrix(m,0x6c0+u*64,generated,out[9+u]);
        else memcpy(out[9+u],generated,4*sizeof(float));
    }
    return 0;
}
