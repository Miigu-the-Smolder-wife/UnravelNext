// unx-kernel: cs_6_6 main
// Camera paths of the reference estimator (Reference/PathTracer/src/PathTracer.cpp, PathTracer::Impl::radiance), one
// thread per (pixel, half) of the dispatch rectangle, samples [sampleBegin, sampleEnd) of that half. The per-thread
// float sum of the dispatch's samples is added to the half's double accumulator (the CPU adds per-pass double sums).
#include "Common.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    uint nans = 0, truncated = 0;
    if (id.x < g_root.w && id.y < g_root.h)
    {
        const uint x = g_root.x0 + id.x, y = g_root.y0 + id.y, half_ = id.z;
        const uint pi = y * C.width + x;
        const uint pixelSeed = rtHashCombine(rtHashCombine(C.seedLo ^ C.seedHi, half_), pi);
        float3 acc = float3(0, 0, 0);
        for (uint si = g_root.sampleBegin; si < g_root.sampleEnd; ++si)
        {
            RtSampler smp = rtSamplerInit(pixelSeed, si);
            float jx, jy;
            rtGet2D(smp, jx, jy);
            const float3 dir = rtCameraRay(C.camera, C.width, C.height, (float)x + jx, (float)y + jy);
            const float tn = C.camera.nearPlane / max(dot(dir, C.camera.forward), 1e-3f);
            bool trunc = false;
            const float3 v = rtRadiance(C, C.camera.position, dir, tn, smp, trunc);
            truncated += trunc ? 1 : 0;
            if (!rtFinite3(v))
            {
                ++nans;
                continue;
            }
            acc += v;
        }
        RWByteAddressBuffer accum = ResourceDescriptorHeap[half_ == 0 ? C.b.accum0 : C.b.accum1];
        rtAccumulateDouble3(accum, pi, acc);
    }
    rtFlushCounters(nans, truncated, 0);
}
