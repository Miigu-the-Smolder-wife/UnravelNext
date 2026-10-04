// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// MODE0: clear count. MODE1: once per cell, visibility/depth constraint, compact
// visible origins and initialize all nine trace texels (hidden cells remain zero).
// MODE2: allocator-driven indirect ray descriptions, bounded exactly as before.
// P[0]: list UAV, depth SRV, HiZ SRV, trace UAV
// P[1]: argument UAV, description stride, Width offset, chunks
// P[2].x: ray threads per chunk; grid/jitter/threshold match the trace kernel.
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"

#if MODE == 1
[numthreads(4, 4, 4)]
void main(uint3 cell : SV_DispatchThreadID)
{
    const uint3 grid = ltvGridSize();
    if (any(cell >= grid)) return;
    RWTexture3D<float3> trace = ResourceDescriptorHeap[P[0].w];
    for (uint y = 0; y < LTV_TRACE_RES; ++y)
        for (uint x = 0; x < LTV_TRACE_RES; ++x)
            trace[uint3(cell.xy * LTV_TRACE_RES + uint2(x, y), cell.z)] = 0;
    const bool visible = ltvCellVisible(cell, P[0].z);
    const uint count = WaveActiveCountBits(visible), prefix = WavePrefixCountBits(visible);
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    uint base = 0;
    if (WaveIsFirstLane() && count) list.InterlockedAdd(0, count, base);
    base = WaveReadLaneFirst(base);
    if (!visible) return;
    float3 offset = ltvFrameJitter();
    ltvDepthConstraint(cell, offset, P[0].y, asfloat(P[9].z));
    const float3 origin = ltvCellPosition(float3(cell) + offset);
    const uint index = cell.x + grid.x * (cell.y + grid.y * cell.z);
    list.Store4(16 + (base + prefix) * 16, uint4(index, asuint(origin)));
}
#else
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
#if MODE == 0
    list.Store4(0, uint4(0, 0, 0, 0));
#else
    const uint3 grid = ltvGridSize();
    const uint rays = min(list.Load(0), grid.x * grid.y * grid.z) * 9u;
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
    for (uint chunk = 0; chunk < P[1].w; ++chunk)
    {
        const uint first = chunk * P[2].x;
        const uint width = rays > first ? min(rays - first, P[2].x) : 0;
        args.Store3(chunk * P[1].y + P[1].z, width ? uint3(width, 1, 1) : uint3(0, 0, 0));
    }
#endif
}
#endif
