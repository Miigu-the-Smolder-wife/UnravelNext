// unx-kernel: cs_6_6 main
// Fluid surface: slot per active block (block order), block per slot, and the indirect dispatch arguments
// (0: active x 512 threads in groups of 256, 1: one group per active block). Blocks past the pool are counted.
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 256);
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[6].x];
    uint n = fsTableSize();
    if (i < n && table.Load(4 * i) != 0)
    {
        uint slot = scan.Load(4 * i) + scan.Load(4 * (FS_SUMS(n) + i / 1024));
        if (slot < fsMaxBlocks()) { table.Store(4 * i, slot + 1); scan.Store(4 * (FS_SLOTS(n) + slot), i); }
        else { table.Store(4 * i, 0); counters.InterlockedAdd(4 * FS_COUNTER_OVERFLOW, 1); }
    }
    if (i == 0)
    {
        uint active = min(counters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks());
        // 1D group counts as rows of WATER_LINEAR_ROW groups (D3D12: at most 65535 per dimension; WaterLinear.hlsli).
        const uint nodes = (active * FS_BLOCK_NODES + 255) / 256;
        args.Store3(0, uint3(min(nodes, WATER_LINEAR_ROW), (nodes + WATER_LINEAR_ROW - 1) / WATER_LINEAR_ROW, 1));
        args.Store3(12, uint3(min(active, WATER_LINEAR_ROW), (active + WATER_LINEAR_ROW - 1) / WATER_LINEAR_ROW, 1));
    }
}
