// unx-kernel: cs_6_6 main
// Water height clipmap bounds (OceanHeight.hlsli): mip 0 = max/min of each cell's 4 corner heights (cells 0..510 of
// every level; cell 511 repeats cell 510's column/row so every mip is a power of two), mip m = max/min of the 4
// children of mip m - 1 (P[3].z = m; P[3].y the source mip's UAV, P[0].z this mip's UAV).
#include "OceanHeight.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint mip = P[3].z, size = OH_N >> mip;
    if (id.x >= size || id.y >= size || id.z >= P[0].w) return;
    RWTexture2DArray<float2> bounds = ResourceDescriptorHeap[P[0].z];
    float2 b;
    if (mip == 0)
    {
        RWTexture2DArray<float4> height = ResourceDescriptorHeap[P[0].y];
        const uint2 cell = min(id.xy, uint2(OH_CELLS - 1, OH_CELLS - 1));
        const float a = height[uint3(cell, id.z)].x, c = height[uint3(cell + uint2(1, 0), id.z)].x;
        const float d = height[uint3(cell + uint2(0, 1), id.z)].x, e = height[uint3(cell + uint2(1, 1), id.z)].x;
        b = float2(max(max(a, c), max(d, e)), min(min(a, c), min(d, e)));
    }
    else
    {
        RWTexture2DArray<float2> source = ResourceDescriptorHeap[P[3].y];
        const uint2 child = id.xy * 2;
        const float2 p = source[uint3(child, id.z)], q = source[uint3(child + uint2(1, 0), id.z)];
        const float2 r = source[uint3(child + uint2(0, 1), id.z)], t = source[uint3(child + uint2(1, 1), id.z)];
        b = float2(max(max(p.x, q.x), max(r.x, t.x)), min(min(p.y, q.y), min(r.y, t.y)));
    }
    bounds[id] = b;
}
