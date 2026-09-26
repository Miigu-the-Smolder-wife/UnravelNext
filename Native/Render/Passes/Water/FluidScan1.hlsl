// unx-kernel: cs_6_6 main
// Fluid surface: active-block scan, level 1 (one group over the <= 1,024 group sums; the total is the active count).
#include "FluidSurface.hlsli"

groupshared uint g_sum[1024];
[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    uint n = fsTableSize(), groups = (n + 1023) / 1024;
    uint v = t < groups ? scan.Load(4 * (FS_SUMS(n) + t)) : 0;
    g_sum[t] = v; GroupMemoryBarrierWithGroupSync();
    for (uint s = 1; s < 1024; s <<= 1) { uint a = t >= s ? g_sum[t - s] : 0; GroupMemoryBarrierWithGroupSync(); g_sum[t] += a; GroupMemoryBarrierWithGroupSync(); }
    if (t < groups) scan.Store(4 * (FS_SUMS(n) + t), g_sum[t] - v);
    if (t == 1023) counters.Store(4 * FS_COUNTER_ACTIVE, g_sum[1023]);
}
