// unx-kernel: cs_6_6 main
// Fluid surface: active-block scan, level 0 (1,024-block groups; exclusive prefix and group sums).
#include "FluidSurface.hlsli"

groupshared uint g_scan[1024];
[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    uint i = g * 1024 + t, n = fsTableSize();
    uint v = i < n && table.Load(4 * i) != 0 ? 1u : 0u;
    g_scan[t] = v; GroupMemoryBarrierWithGroupSync();
    for (uint s = 1; s < 1024; s <<= 1) { uint a = t >= s ? g_scan[t - s] : 0; GroupMemoryBarrierWithGroupSync(); g_scan[t] += a; GroupMemoryBarrierWithGroupSync(); }
    if (i < n) scan.Store(4 * i, g_scan[t] - v);
    if (t == 1023) scan.Store(4 * (FS_SUMS(n) + g), g_scan[1023]);
}
