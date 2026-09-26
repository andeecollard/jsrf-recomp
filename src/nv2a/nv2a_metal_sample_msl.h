#ifndef NV2A_METAL_SAMPLE_MSL_H
#define NV2A_METAL_SAMPLE_MSL_H
/* THE EXECUTOR'S BUFFER SAMPLER, AS MSL SOURCE, FOR TWO SHADERS.
 *
 * nv2a_metal.m samples a guest texture by hand from its bytes -- texel(),
 * sample_level(), sample_at(), sample_lod() and BUMPENVMAP's bump_sample() --
 * whenever the sampler hardware cannot stand in (a bump unit always; see
 * the comments beside the macro's use there). G75 draws bump-mapped meshes
 * on the host (d3d8_host_2d_metal.m), and the host must displace exactly as
 * the executor does, so both shaders take these functions from here rather
 * than the host keeping a second copy.
 *
 * P names the MSL struct the functions read through `constant P&s`. It must
 * have these uint[4] members: tw th pitch linear rgba8 dxt1 dxt3 sz16 lin32
 * repeat levels min_filter bump; float[4]: lod_bias bump_scale bump_offset;
 * and float bump_mat[16]. The executor passes its Params, and the text it
 * compiles is the text it compiled before the move, byte for byte. */
#define NV2A_MSL_BUFFER_SAMPLER(P) \
    "uint morton(uint x,uint y,uint w,uint h) { uint index=0,bit=0;" \
    " for(uint b=1;b<w||b<h;b<<=1) { if(b<w){if(x&b)index|=1u<<bit;bit++;}" \
    " if(b<h){if(y&b)index|=1u<<bit;bit++;}} return index; }\n" \
    "float4 texel(const device uchar *t,int2 p,uint u,uint base,uint w,uint h,uint pitch,constant " P "&s){\n" \
    " if(s.repeat[u]){p.x=(p.x%int(w)+int(w))%int(w);p.y=(p.y%int(h)+int(h))%int(h);}" \
    " else p=clamp(p,int2(0),int2(w-1,h-1));" \
    " uint at=base+(s.rgba8[u]?4*morton(uint(p.x),uint(p.y),w,h):s.dxt1[u]?uint(p.y/4)*pitch+uint(p.x/4)*8:s.dxt3[u]?uint(p.y/4)*pitch+uint(p.x/4)*16:s.sz16[u]?2*morton(uint(p.x),uint(p.y),w,h):uint(p.y)*pitch+uint(p.x)*(s.lin32[u]?4u:2u));\n" \
    " if(s.rgba8[u])return float4(float(t[at+2]),float(t[at+1]),float(t[at]),s.rgba8[u]==2u?255.0f:float(t[at+3]))/255;" \
    " if(s.lin32[u])return float4(float(t[at+2]),float(t[at+1]),float(t[at]),s.lin32[u]==2u?255.0f:float(t[at+3]))/255;" \
    " if(s.sz16[u]){uint c=uint(t[at])|(uint(t[at+1])<<8);" \
    " return s.sz16[u]==2u?float4(float((c>>8)&15),float((c>>4)&15),float(c&15),float((c>>12)&15))/15" \
    ":float4(float((c>>10)&31)/31,float((c>>5)&31)/31,float(c&31)/31,1);}" \
    " if(s.dxt1[u]){uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);" \
    " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*((p.y&3)*4+(p.x&3)))&3;" \
    " uint c=pick?c1:c0;float4 a=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);if(pick<2)return a;" \
    " if(c0<=c1&&pick==3)return float4(0);c=c0;float4 x=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);" \
    " c=c1;float4 y=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);float w=c0<=c1?.5f:(pick==2?2.0f/3.0f:1.0f/3.0f);return float4(x.rgb*w+y.rgb*(1-w),1);}" \
    " if(s.dxt3[u]){uint i=uint(p.y&3)*4+uint(p.x&3),a=(uint(t[at+i/2])>>(4*(i&1)))&15;at+=8;" \
    " uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);" \
    " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*i)&3;" \
    " uint c=pick?c1:c0;float3 x=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);if(pick<2)return float4(x,float(a)/15);" \
    " c=c0;float3 r=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);c=c1;float3 b=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);" \
    " float w=pick==2?2.0f/3.0f:1.0f/3.0f;return float4(r*w+b*(1-w),float(a)/15);}" \
    " uint c=uint(t[at])|(uint(t[at+1])<<8);return float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);}\n" \
    "float4 sample_level(const device uchar*t,float2 uv,uint u,uint level,bool linear,constant " P "&s){" \
    " uint base=0,w=s.tw[u],h=s.th[u],pitch=s.pitch[u];for(uint l=0;l<level;l++){" \
    " base+=s.dxt1[u]?((w+3)/4)*((h+3)/4)*8:s.dxt3[u]?((w+3)/4)*((h+3)/4)*16:s.rgba8[u]?w*h*4:s.sz16[u]?w*h*2:pitch*h;w=max(1u,w/2);h=max(1u,h/2);pitch=s.dxt1[u]?((w+3)/4)*8:s.dxt3[u]?((w+3)/4)*16:s.rgba8[u]?w*4:s.sz16[u]?w*2:pitch;}" \
    " if(s.rgba8[u]||s.dxt1[u]||s.dxt3[u]||s.sz16[u]){if(s.repeat[u])uv-=floor(uv);else uv=clamp(uv,float2(0),float2(1));uv*=float2(w,h);}" \
    " uv=clamp(uv,float2(0),float2(w,h));if(!linear)return texel(t,int2(floor(uv)),u,base,w,h,pitch,s);" \
    " float2 p=uv-.5f,f=floor(p),fxy=p-f;int2 q=int2(f);" \
    " return(texel(t,q,u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,0),u,base,w,h,pitch,s)*fxy.x)*(1-fxy.y)" \
    " +(texel(t,q+int2(0,1),u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,1),u,base,w,h,pitch,s)*fxy.x)*fxy.y;}\n" \
    "float4 sample_at(const device uchar*t,float2 uv,float2 luv,uint u,constant " P "&s){" \
    " float2 scale=float2(s.tw[u],s.th[u]);float lod=log2(max(0.000001f,max(length(dfdx(luv)*scale),length(dfdy(luv)*scale))));" \
    " float l=max(0.0f,lod+s.lod_bias[u]);if(s.min_filter[u]<3||s.levels[u]<2)l=0;" \
    " l=min(l,float(s.levels[u]-1));uint lo=s.min_filter[u]>=5?uint(floor(l)):uint(floor(l+.5f));" \
    " uint hi=s.min_filter[u]>=5&&lo+1<s.levels[u]?lo+1:lo;bool linear=s.linear[u]!=0;" \
    " if(lod+s.lod_bias[u]>0&&s.min_filter[u])linear=(s.min_filter[u]&1)==0;" \
    " float4 a=sample_level(t,uv,u,lo,linear,s),b=hi==lo?a:sample_level(t,uv,u,hi,linear,s);" \
    " return mix(a,b,hi==lo?0.0f:l-float(lo));}\n" \
    "float4 sample_lod(const device uchar*t,float4 tc,uint u,constant " P "&s){float2 uv=tc.xy/tc.w;return sample_at(t,uv,uv,u,s);}\n" \
    "float sign3(float x){x*=255.0f;return x>=128.0f?(x-256.0f)/127.0f:x/127.0f;}\n" \
    "float4 bump_sample(const device uchar*t,float4 tc,uint u,float4 src,constant " P "&s){" \
    " float du=sign3(src.b),dv=sign3(src.g);" \
    " float pu=s.bump_mat[4*u]*du+s.bump_mat[4*u+2]*dv,pv=s.bump_mat[4*u+1]*du+s.bump_mat[4*u+3]*dv;" \
    " float4 c=sample_at(t,tc.xy+float2(pu,pv),tc.xy,u,s);" \
    " if(s.bump[u]==7u)c*=s.bump_scale[u]*src.r+s.bump_offset[u];return c;}\n"

#endif
