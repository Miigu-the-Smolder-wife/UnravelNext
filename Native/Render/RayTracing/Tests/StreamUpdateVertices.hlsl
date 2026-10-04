// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint cells = P[0].y, total = cells * cells * 6;
    if (i >= total) return;
    const uint2 corners[6] = {uint2(0,0), uint2(0,1), uint2(1,0), uint2(1,0), uint2(0,1), uint2(1,1)};
    const uint cell = i / 6;
    const uint2 node = uint2(cell % cells, cell / cells) + corners[i % 6];
    float3 p = float3(float(P[0].z) * 3 + 2.0 * node.x / cells - 1, asfloat(P[1].x), 2.0 * node.y / cells - 1);
    if ((P[0].w & 4u) != 0) p.y += 0.04 * sin(p.x * 3.0 + asfloat(P[1].x) * 7) * cos(p.z * 4.0 - asfloat(P[1].x) * 5);
    if ((P[0].w & 1u) != 0 && i >= total - 3) p.x = asfloat(0x7fc00000u);
    if ((P[0].w & 2u) != 0 && i < 3) p = float3(float(P[0].z) * 3 - 1, asfloat(P[1].x), -1);
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].x];
    vertices.Store4(i * 32, asuint(float4(p, 1)));
    vertices.Store4(i * 32 + 16, asuint(float4(0, 1, 0, 0)));
}
