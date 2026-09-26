// unx-kernel: cs_6_6 main
// Fluid surface: one group per active block (indirect, argument 1), one thread per marching cubes cell: the cell's case
// against the 0.5 level and its triangle count, a block prefix sum (the cell's first triangle in the block), and the
// block total.
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

groupshared uint g_density[FS_WINDOW * FS_WINDOW * FS_WINDOW];
groupshared uint g_prefix[FS_BLOCK_NODES];
[numthreads(512, 1, 1)]
void main(uint t : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint g = group.y * WATER_LINEAR_ROW + group.x;  // the active block (rows of WATER_LINEAR_ROW groups)
    RWByteAddressBuffer blockCounters = ResourceDescriptorHeap[P[4].w];
    if (g >= min(blockCounters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks())) return;  // the last row's extra groups
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer info = ResourceDescriptorHeap[P[5].x];
    RWByteAddressBuffer blockTris = ResourceDescriptorHeap[P[5].y];
    ByteAddressBuffer cases = ResourceDescriptorHeap[P[5].w];
    int3 origin = fsBlockCoord(scan.Load(4 * (FS_SLOTS(fsTableSize()) + g))) * 8 - 1;
    for (uint k = t; k < FS_WINDOW * FS_WINDOW * FS_WINDOW; k += FS_BLOCK_NODES)
        g_density[k] = fsDensity(origin + int3(k % FS_WINDOW, (k / FS_WINDOW) % FS_WINDOW, k / (FS_WINDOW * FS_WINDOW)));
    GroupMemoryBarrierWithGroupSync();
    int3 c = int3(t % 8, (t / 8) % 8, t / 64) + 1;
    uint mask = 0;
    [unroll] for (uint corner = 0; corner < 8; ++corner)
    {
        int3 n = c + fsCorner(corner);
        if (g_density[(n.z * FS_WINDOW + n.y) * FS_WINDOW + n.x] >= FS_ISO) mask |= 1u << corner;
    }
    uint count = cases.Load(4 * mask * FS_CASE_STRIDE);
    g_prefix[t] = count; GroupMemoryBarrierWithGroupSync();
    for (uint s = 1; s < FS_BLOCK_NODES; s <<= 1) { uint a = t >= s ? g_prefix[t - s] : 0; GroupMemoryBarrierWithGroupSync(); g_prefix[t] += a; GroupMemoryBarrierWithGroupSync(); }
    info.Store(4 * (g * FS_BLOCK_NODES + t), mask | ((g_prefix[t] - count) << 8));
    if (t == FS_BLOCK_NODES - 1) blockTris.Store(4 * g, g_prefix[t]);
}
