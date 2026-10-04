// unx-kernel: cs_6_6 main
// Fluid surface: retire the triangles past the drawn ones that an earlier record drew (FS_COUNTER_TAIL_FROM/TO, written by
// FluidBlockScan.hlsl; the first record retires the whole capacity). Their vertices get NaN positions: rasterisation
// draws only the drawn count, but a ray tracing build over the whole capacity (R: the stream's BLAS at maxTriangles)
// treats a triangle whose vertex x is NaN as inactive. Refittable streams instead use three identical finite positions:
// degenerate triangles never intersect rays, but unlike NaN-inactive primitives may become nondegenerate on DXR update.
// One thread per retired triangle; FluidBlockScan writes the indirect dispatch.
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint local = waterLinear(group, thread, 256);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    const uint first = counters.Load(4 * FS_COUNTER_TAIL_FROM), end = counters.Load(4 * FS_COUNTER_TAIL_TO);
    if (local >= end - first) return;
    const uint i = first + local;
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[5].z];
    float3 retirementPosition = asfloat(uint3(0x7FC00000u, 0x7FC00000u, 0x7FC00000u));
    if (fsRefittableTail())
    {
        retirementPosition = fsOrigin() * fsAxes();
        if (first != 0) retirementPosition = asfloat(vertices.Load3((3 * (first - 1)) * 32));
    }
    [unroll] for (uint v = 0; v < 3; ++v) vertices.Store4((3 * i + v) * 32, asuint(float4(retirementPosition, 1)));
}
