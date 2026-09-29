// unx-kernel: cs_6_6 main
// renderergate --luminance-log: the frame's mean luminance of the gate output, per frame, for drift and run-to-run
// comparisons (the brightness of a still scene over a run). Integer sums (the order of the atomics does not change them):
// per pixel round(4096 x L), L = exposed-linear Rec. 709 luminance. No tone map or UNORM clamp.
// Each 16-byte slot contains a uint64 sum and a uint invalid-pixel count. Values outside
// [0, 2^24) are reported as invalid rather than silently saturating. At <= 2^24 pixels
// the fixed-point sum fits uint64, including a 64-lane wave's sum.
// P[0] = { output SRV (Texture2D<float4>), sums UAV (raw), slot, width }, P[1] = { height, unused,
// clear (1: one thread zeroes the slot), 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 px : SV_DispatchThreadID)
{
    RWByteAddressBuffer sums = ResourceDescriptorHeap[P[0].y];
    if (P[1].z != 0)
    {
        if (all(px == 0)) sums.Store4(P[0].z * 16, 0u);
        return;
    }
    uint64_t v = 0;
    if (px.x < P[0].w && px.y < P[1].x)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[P[0].x];
        const float3 c = t.Load(int3(px, 0)).rgb;
        float l = dot(max(c, 0), float3(0.2126, 0.7152, 0.0722));
        if (all(isfinite(c)) && isfinite(l) && l < 16777216.0)
            v = (uint64_t)round(l * 4096.0);
        else
            sums.InterlockedAdd(P[0].z * 16 + 8, 1u);
    }
    const uint64_t s = WaveActiveSum(v);
    if (WaveIsFirstLane() && s != 0)
    {
        uint64_t unused;
        sums.InterlockedAdd64(P[0].z * 16, s, unused);
    }
}
