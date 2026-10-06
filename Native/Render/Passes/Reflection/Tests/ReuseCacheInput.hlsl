// unx-kernel: cs_6_6 main
#include "Passes/Reflection/ReflectionInternal.hlsli"
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[1].xy)) return;
    RWTexture2D<uint> modes = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
    uint job = p.y * P[1].x + p.x;
    const bool traced = P[1].z == 1 || all((p & 1u) == uint2(P[1].w & 1u, (P[1].w >> 1) & 1u));
    if (P[2].x == 0) modes[p] = depth[p] > 0 && ((p.x / 17 + p.y / 19) % 7 != 0) ? REFL_M | ((traced ? job : REFL_NO_JOB) << 8) : 0;
    else job = reflMode(modes[p]) == REFL_M ? reflJob(modes[p]) : REFL_NO_JOB;
    if (job != REFL_NO_JOB) results[job] = reflPackResult(float3(1 + (p.x % 29), 0.3 + (p.y % 37), 3 + ((p.x + p.y) % 17)), 0.2 + (job % 223), 0);
    if (all((p & 7u) == 0)) reflection[uint2(p.x / 8, P[1].y + p.y / 8)] = float4(0, 0, 0, 1);
}
