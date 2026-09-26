// unx-kernel: cs_6_6 main
// The halves' relMSE (GpuPathTracer::currentHalvesRelMse): one group adds Mean.hlsl's per-group double sums in a fixed
// order (thread t adds groups t, t + 1024, ... in turn, then a fixed tree), times 1 / (pixels x 3) - so the value is the
// same on every run; it equals metrics::relMse(halfA, halfB) up to the order of the double additions.
// Root: x0 = the group sums (RWByteAddressBuffer, y0 doubles), w = the result's UAV (RWByteAddressBuffer, one double at 0),
// h / sampleBegin = 1 / (pixels x 3) (double bits lo / hi).
#include "Common.hlsli"

groupshared double gs_sum[1024];

[numthreads(1024, 1, 1)]
void main(uint flat : SV_GroupIndex)
{
    RWByteAddressBuffer sums = ResourceDescriptorHeap[g_root.x0];
    const uint count = g_root.y0;
    double s = 0;
    for (uint i = flat; i < count; i += 1024)
    {
        const uint2 v = sums.Load2(i * 8);
        s += asdouble(v.x, v.y);
    }
    gs_sum[flat] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 512; k > 0; k >>= 1)
    {
        if (flat < k) gs_sum[flat] += gs_sum[flat + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (flat == 0)
    {
        RWByteAddressBuffer result = ResourceDescriptorHeap[g_root.w];
        uint lo, hi;
        asuint(gs_sum[0] * asdouble(g_root.h, g_root.sampleBegin), lo, hi);
        result.Store2(0, uint2(lo, hi));
    }
}
