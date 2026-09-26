// unx-kernel: cs_6_6 main
// Fluid surface: one group scans the active blocks' triangle counts in block order (first triangle per block), writes
// the total and the draw arguments (vertices = 3 x triangles, capped at the capacity; a cut is counted).
#include "FluidSurface.hlsli"

groupshared uint g_scan[1024];
[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    RWByteAddressBuffer blockTris = ResourceDescriptorHeap[P[5].y];
    RWByteAddressBuffer draw = ResourceDescriptorHeap[P[6].y];
    uint active = min(counters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks()), carry = 0;
    for (uint base = 0; base < active; base += 1024)
    {
        uint i = base + t, v = i < active ? blockTris.Load(4 * i) : 0;
        g_scan[t] = v; GroupMemoryBarrierWithGroupSync();
        for (uint s = 1; s < 1024; s <<= 1) { uint a = t >= s ? g_scan[t - s] : 0; GroupMemoryBarrierWithGroupSync(); g_scan[t] += a; GroupMemoryBarrierWithGroupSync(); }
        if (i < active) blockTris.Store(4 * (fsMaxBlocks() + i), carry + g_scan[t] - v);
        uint total = g_scan[1023];
        GroupMemoryBarrierWithGroupSync();
        carry += total;
    }
    if (t == 0)
    {
        counters.Store(4 * FS_COUNTER_TRIANGLES, carry);
        if (carry > fsMaxTriangles()) counters.InterlockedAdd(4 * FS_COUNTER_OVERFLOW, 1);
        draw.Store4(0, uint4(min(carry, fsMaxTriangles()) * 3, 1, 0, 0));
    }
}
