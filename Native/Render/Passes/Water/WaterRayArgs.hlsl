// unx-kernel: cs_6_6 main
// Water stage 3: the apply pass's indirect dispatch over the band's samples (WaterRayApply.hlsl, 64 per group, rows of
// WATER_LINEAR_ROW groups), from the sample list's count capped at its capacity.
// P[0] samples UAV (raw: head { count }), dispatch arguments UAV (raw, 12 B), sample capacity, 0
#include "Bindless.hlsli"
#include "WaterLinear.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer samples = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint count = min(samples.Load(0), P[0].z), groups = (count + 63) / 64;
    samples.Store(4, count);
    args.Store3(0, uint3(min(groups, WATER_LINEAR_ROW), (groups + WATER_LINEAR_ROW - 1) / WATER_LINEAR_ROW, 1));
}
