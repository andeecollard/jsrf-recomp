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
static float clamp01(float x)
{ return x<0?0:x>1?1:x; }
const char *nv2a_ff_vertex(const uint32_t m[2048],const float in[16][4],float out[16][4])
{
    static const uint32_t normal_map=0x8511;
    if(m[0x328/4]) return "fixed-function skinning";
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
    if(m[0x314/4]) {
        /* JSRF's first previously rejected fixed-function draws use the
         * NV2A infinite-light path. Diffuse colour supplies the material;
         * scene/light ambient and N.L directional diffuse produce D0. Keep
         * local/spot lights explicit until their attenuation state is seen. */
        uint32_t light_mask=m[0x3bc/4];
        for(unsigned light=0;light<8;++light)
            if(((light_mask>>(2*light))&3)>1) return "fixed-function local / spot light";
        for(unsigned k=0;k<3;++k) {
            float illumination=value(m,0xa10+4*k);
            for(unsigned light=0;light<8;++light) if(((light_mask>>(2*light))&3)==1) {
                unsigned base=0x1000+light*0x80;
                float direction[3]={value(m,base+0x34),value(m,base+0x38),value(m,base+0x3c)};
                float ndotl=fmaxf(0,-(normal[0]*direction[0]+normal[1]*direction[1]+normal[2]*direction[2]));
                illumination+=value(m,base+4*k)+ndotl*value(m,base+0x0c+4*k);
            }
            out[3][k]=clamp01(value(m,0x3a8+4*k)+in[3][k]*illumination);
        }
        out[3][3]=clamp01(in[3][3]*value(m,0x3b4));
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
