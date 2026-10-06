// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
[numthreads(8,8,1)]
void main(uint2 p : SV_DispatchThreadID)
{
    const uint2 size=P[4].xy;
    if(any(p>=size))return;
    const uint seed=P[4].z, i=p.x+p.y*size.x;
    uint h=(i+seed*977u)*1664525u+1013904223u; h^=h>>16; h*=2246822519u;
    const float3 c=float3(h&1023u,(h>>10)&1023u,(h>>20)&1023u)/1023.0;
    const float scale=(seed&2u)?32.0:0.1;
    const bool sky=P[4].w==0 && (i%17u==0 || ((p.x/8u+p.y/8u+seed)%5u)==0);
    float3 current=c*scale;
    if ((p.x+seed)%29u==0) current.x=60000;
    if ((p.y+seed)%31u==0) current.y=0.0001;
    if (P[4].w==0 && (seed&4u)!=0 && i%83u==0) current=asfloat(uint3(0x7fc00000u,0x7f800000u,0xff800000u));
    RWTexture2D<float4> d=ResourceDescriptorHeap[P[0].x],s=ResourceDescriptorHeap[P[0].y];
    d[p]=float4(current,(h&255u)/255.0);
    s[p]=float4(c.brg*scale,sky||i%13u==0?0:1);
    RWTexture2D<float> depth=ResourceDescriptorHeap[P[0].z];
    const float z=0.1/(2.0+float(p.x%19u)/19.0); depth[p]=z;
    GBufferSample gb; gb.normal=normalize(float3(c.xy*0.4-0.2,1));gb.baseColor=c;gb.roughness=0.1+0.8*c.y;
    RWTexture2D<uint2> buffer=ResourceDescriptorHeap[P[0].w];buffer[p]=encodeGBuffer(gb);
    RWTexture2D<float4> pd=ResourceDescriptorHeap[P[1].x],ps=ResourceDescriptorHeap[P[1].y],pm=ResourceDescriptorHeap[P[1].z];
    pd[p]=float4(c.gbr*scale,1);ps[p]=float4(c*scale,1);pm[p]=float4(c.xy*scale,c.zx*scale);
    RWTexture2D<uint> pf=ResourceDescriptorHeap[P[1].w];pf[p]=(i+seed)%11u==0?0:8u+(h&127u);
    RWTexture2D<float> key=ResourceDescriptorHeap[P[2].x];key[p]=linearDepth(z)*(i%23u==0?2.0:1.0);
    RWTexture2D<uint> word=ResourceDescriptorHeap[P[2].y];word[p]=sky?0xFFFFu:(i%4u)|((h&255u)<<16)|(((h>>8)&127u)<<24);
    RWTexture2D<float4> gi=ResourceDescriptorHeap[P[2].z],rough=ResourceDescriptorHeap[P[2].w];
    gi[p]=float4(c*scale,1);rough[p]=float4(c.grb*scale,1);
    RWTexture2D<float4> ao=ResourceDescriptorHeap[P[3].x],emission=ResourceDescriptorHeap[P[3].y];
    ao[p]=float4(gb.normal*(0.1+0.9*c.z),1);emission[p]=float4(c.bgr*scale,1);
}
