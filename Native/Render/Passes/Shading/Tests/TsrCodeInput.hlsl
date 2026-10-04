// unx-kernel: cs_6_6 main
#include "Passes/Shading/Tsr.hlsli"
[numthreads(8,8,1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[2].xy)) return;
    uint i = p.y * P[2].x + p.x, seed = P[2].z;
    uint n = i * 1664525u + seed * 1013904223u;
    float3 code = float3(i & 2047u, (i * 17u + seed) & 2047u, (i * 37u) & 1023u) / float3(2047,2047,1023);
    float3 c = tsrGuideToLinear(code);
    if (seed & 1u) c = float3(n & 65535u, (n >> 8) & 65535u, (n >> 16) & 65535u) / 127.0;
    if (i % 71u == 0) c = asfloat(uint3(0x7fc00000, 0x7f800000, 0xff800000));
    if (i % 73u == 0) c = -1;
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x]; colour[p] = float4(c,1);
    RWTexture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    guide[p] = float4(float3(n & 1023u, (n >> 10) & 1023u, (n >> 20) & 1023u) / 1023.0, (i % 4u) / 3.0);
    RWTexture2D<float2> mask = ResourceDescriptorHeap[P[0].z]; mask[p] = float2(i % 32u, (i * 17u) % 256u) / 255.0;
    RWTexture2D<float> moire = ResourceDescriptorHeap[P[0].w]; moire[p] = (i % 16u) / 32.0;
    RWTexture2D<float> thin = ResourceDescriptorHeap[P[1].x]; thin[p] = (i % 7u) / 6.0;
    RWTexture2D<float4> layers = ResourceDescriptorHeap[P[1].y]; layers[p] = float4(0,(i % 11u)/10.0,(i%13u)/12.0,1);
    RWTexture2D<float4> measure = ResourceDescriptorHeap[P[1].z]; measure[p] = float4((i%17u)/16.0,0.7,(i%2u),1);
    RWTexture2D<float4> resurrect = ResourceDescriptorHeap[P[1].w]; resurrect[p] = float4(code, (i%4u)/3.0);
}
