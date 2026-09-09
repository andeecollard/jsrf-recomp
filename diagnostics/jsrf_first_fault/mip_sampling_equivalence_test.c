/* White-box equivalence check for the single-mip sampling optimisation. */
#include "../../src/nv2a/nv2a_texture_copy.c"
#include <stdio.h>

static void reference(const NV2ATextureCopy *s, const uint8_t *data,
                      float u, float v, float lod, float out[4])
{
    float l=fmaxf(0,lod+s->lod_bias);
    if(s->min_filter<3 || s->levels<2) l=0;
    l=fminf(l,(float)(s->levels?s->levels-1:0));
    unsigned lo=(unsigned)(s->min_filter>=5?floorf(l):floorf(l+.5f));
    unsigned hi=s->min_filter>=5 && lo+1<s->levels?lo+1:lo;
    NV2ATextureCopy t=*s;
    if(lod+s->lod_bias>0 && s->min_filter) t.linear=(s->min_filter%2)==0;
    const uint8_t *p=data;
    float a[4],b[4];
    for(unsigned level=0;level<=hi;++level) {
        if(level==lo) sample(&t,p,u,v,a);
        if(level==hi) { sample(&t,p,u,v,b); break; }
        p+=t.dxt1?(size_t)((t.width+3)/4)*((t.height+3)/4)*8:(size_t)t.width*t.height*4;
        t.width=t.width>1?t.width/2:1; t.height=t.height>1?t.height/2:1;
        t.pitch=t.dxt1?((t.width+3)/4)*8:t.width*4;
    }
    float f=hi==lo?0:l-lo;
    for(unsigned k=0;k<4;++k) out[k]=a[k]*(1-f)+b[k]*f;
}

int main(void)
{
    uint8_t data[256];
    for(unsigned i=0;i<sizeof(data);++i) data[i]=(uint8_t)(i*73+19);
    unsigned checks=0;
    for(unsigned format=0;format<2;++format)
    for(unsigned levels=1;levels<=3;++levels)
    for(unsigned filter=0;filter<=6;++filter)
    for(unsigned repeat=0;repeat<2;++repeat)
    for(unsigned linear=0;linear<2;++linear)
    for(int step=-4;step<=12;++step) {
        NV2ATextureCopy s={0};
        s.width=s.height=4;s.pitch=format?8:16;
        s.dxt1=format;s.rgba8=!format;s.levels=levels;
        s.min_filter=filter;s.repeat=repeat;s.linear=linear;s.lod_bias=.125f;
        float actual[4],expected[4];
        float u=step*.137f,v=1-step*.213f,lod=step*.25f;
        sample_lod(&s,data,u,v,lod,actual);
        reference(&s,data,u,v,lod,expected);
        if(memcmp(actual,expected,sizeof(actual))) {
            fprintf(stderr,"mip mismatch: format=%u levels=%u filter=%u step=%d\n",format,levels,filter,step);
            return 1;
        }
        ++checks;
    }
    printf("%u mip samples bit-identical to pre-optimisation reference\n",checks);
    return 0;
}
