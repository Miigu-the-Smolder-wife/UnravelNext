// unx-kernel: cs_6_6 main
// renderergate --luminance-log: the frame's mean luminance of the gate output, per frame, for drift and run-to-run
// comparisons (the brightness of a still scene over a run). Integer sums (the order of the atomics does not change them):
// per pixel round(1023 x L), L = the Rec. 709 luminance of the output (display-encoded unorm) or L / (1 + L) of a linear
// output; wave sums, then one 64-bit atomic per wave into the frame's slot.
// P[0] = { output SRV (Texture2D<float4>), sums UAV (raw, uint64 per slot), slot, width }, P[1] = { height, linear (1),
// clear (1: one thread zeroes the slot), 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    RWByteAddressBuffer sums = ResourceDescriptorHeap[P[0].y];
    if (P[1].z != 0)
    {
        if (all(px == 0)) sums.Store2(P[0].z * 8, uint2(0, 0));
        return;
    }
    uint v = 0;
    if (px.x < P[0].w && px.y < P[1].x)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[P[0].x];
        const float3 c = t.Load(int3(px, 0)).rgb;
        float l = dot(max(c, 0), float3(0.2126, 0.7152, 0.0722));
        if (P[1].y != 0) l = l / (1 + l);
        v = isfinite(l) ? (uint)round(saturate(l) * 1023.0) : 0u;
    }
    const uint s = WaveActiveSum(v);
    if (WaveIsFirstLane() && s != 0)
    {
        uint64_t unused;
        sums.InterlockedAdd64(P[0].z * 8, (uint64_t)s, unused);
    }
}
