// unx-kernel: cs_6_6 main
// Fluid surface: retire the triangles past the drawn ones that an earlier record drew (FS_COUNTER_TAIL_FROM/TO, written by
// FluidBlockScan.hlsl; the first record retires the whole capacity). Their vertices get NaN positions: rasterisation
// draws only the drawn count, but a ray tracing build over the whole capacity (R: the stream's BLAS at maxTriangles)
// treats a triangle whose vertex x is NaN as inactive (D3D12 raytracing spec), so no stale surface is ever hit.
// One thread per triangle of the capacity (a bounded dispatch; threads outside the range exit).
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 256);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (i < counters.Load(4 * FS_COUNTER_TAIL_FROM) || i >= counters.Load(4 * FS_COUNTER_TAIL_TO)) return;
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[5].z];
    const float nan = asfloat(0x7FC00000u);
    [unroll] for (uint v = 0; v < 3; ++v) vertices.Store4((3 * i + v) * 32, asuint(float4(nan, nan, nan, 1)));
}
