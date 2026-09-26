// unx-kernel: cs_6_6 main
// Core: origin rebase (C9, GpuScene::rebase): every instance's current and previous translation and its break centre
// move by -shift, in place (the GPU copy of the table; the CPU mirror moved the same way).
//   P[0] instances UAV (raw), instance count, shift.xy; P[1] shift.z, instance bytes, break centre offset
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    RWByteAddressBuffer instances = ResourceDescriptorHeap[P[0].x];
    const float3 shift = float3(asfloat(P[0].z), asfloat(P[0].w), asfloat(P[1].x));
    const uint base = i * P[1].y;
    // objectToWorld rows 0..2 (w = translation), prevObjectToWorld rows 3..5; the break centre at P[1].z.
    [unroll] for (uint r = 0; r < 6; ++r)
    {
        const uint at = base + 16 * r + 12;
        instances.Store(at, asuint(asfloat(instances.Load(at)) - shift[r % 3]));
    }
    const uint c = base + P[1].z;
    instances.Store3(c, asuint(asfloat(instances.Load3(c)) - shift));
}
