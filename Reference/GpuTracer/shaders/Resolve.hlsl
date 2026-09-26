// unx-kernel: cs_6_6 main
// Adds the pass's sun-caustic splats (float3 per pixel and half) into the halves' double accumulators and clears the
// splat buffers. One thread per pixel of the dispatch rectangle, both halves.
#include "Common.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    const uint pixel = g_root.pathBase + id.x;
    if (id.x < g_root.pathCount)
    {
        for (uint half_ = 0; half_ < 2; ++half_)
        {
            RWByteAddressBuffer splat = ResourceDescriptorHeap[half_ == 0 ? C.b.splat0 : C.b.splat1];
            RWByteAddressBuffer accum = ResourceDescriptorHeap[half_ == 0 ? C.b.accum0 : C.b.accum1];
            const uint3 bits = splat.Load3(pixel * 12);
            const float3 v = asfloat(bits);
            if (v.x != 0 || v.y != 0 || v.z != 0)
            {
                rtAccumulateDouble3(accum, pixel, v);
                splat.Store3(pixel * 12, uint3(0, 0, 0));
            }
        }
    }
}
