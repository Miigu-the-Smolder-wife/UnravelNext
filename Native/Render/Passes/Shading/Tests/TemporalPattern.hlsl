// unx-kernel: cs_6_6 main
#include "Passes/Shading/Tsr.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[3].xy)) return;
    const uint i = p.y * P[3].x + p.x, seed = P[3].z;
    uint n = (i + seed * 977u) * 1664525u + 1013904223u;
    n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    float3 c = float3(n & 1023u, (n >> 10) & 1023u, (n >> 20) & 1023u) / 1023.0;
    c *= ((p.x + seed) % 13 == 0) ? 65504.0 : ((seed & 1u) ? 16.0 : 0.05);
    float3 kept = tsrLinearToGuide(c);
    const float3 previous = (seed & 2u) ? kept : float3((n >> 20) & 1023u, n & 1023u, (n >> 10) & 1023u) / 1023.0;
    if (i % 71u == 0) c = asfloat(uint3(0x7fc00000, 0x7f800000, 0xff800000));
    if (i % 73u == 0) c = -1;
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x]; colour[p] = float4(c, 1);
    RWTexture2D<float4> prev = ResourceDescriptorHeap[P[0].y]; prev[p] = float4(previous, 1);
    RWTexture2D<float4> resurrect = ResourceDescriptorHeap[P[0].z]; resurrect[p] = float4(kept, 1);
    RWTexture2D<float2> mask = ResourceDescriptorHeap[P[0].w]; mask[p] = float2(n & 31u, (n >> 5) & 255u) / 255.0;
    RWTexture2D<float4> flicker = ResourceDescriptorHeap[P[1].x]; flicker[p] = float4(n & 255u, (n >> 8) & 255u, (n >> 16) & 255u, n >> 24) / 255.0;
    RWTexture2D<float4> info = ResourceDescriptorHeap[P[1].y]; info[p] = float4(0, 0, 0, (i % 8u) / 8.0);
    RWTexture2D<float> relax = ResourceDescriptorHeap[P[1].z]; relax[p] = (i % 7u) / 6.0;
    RWTexture2D<float4> layers = ResourceDescriptorHeap[P[1].w]; layers[p] = float4((n & 255u) / 255.0, (i % 11u) == 0, 0, 1);
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[2].x]; depth[p] = (i % 53u) == 0 ? 0 : 0.1 / (1.0 + 0.1 * (p.y % 10u) + (p.x % 13u == 0 ? 0 : 10.0));
    RWTexture2D<float> history = ResourceDescriptorHeap[P[2].y]; history[p] = (n & 255u) / 255.0;
}
