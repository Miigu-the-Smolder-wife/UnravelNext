// unx-kernel: lib_6_6 main
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/Lumen/LgTraceCache.hlsli"

// The pre-change production algorithm, independent of the optimized helper.
RtHit referenceTrace(RtSceneSrvs scene, inout RayDesc r, uint paramsSrv, uint indirectionSrv, uint atlasSrv, uint depthSrv,
                     float3 position, float dither, out LrcCoverage coverage, out float4 cached)
{
    coverage = (LrcCoverage)0; cached = 0;
    if (paramsSrv != 0xFFFFFFFFu)
    {
        const LrcParams rc = lrcParams(paramsSrv);
        coverage = lrcCoverageChecked(rc, indirectionSrv, position, dither);
        if (coverage.valid)
        {
            cached = lrcSample(rc, indirectionSrv, atlasSrv, depthSrv, coverage, position, r.Direction, lrcSeenFrom(rc, coverage, r.Origin, r.Direction));
            coverage.valid = cached.a > 0;
        }
        if (coverage.valid) r.TMax = min(r.TMax, coverage.minTraceDistance);
    }
    return rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER | RT_MASK_FAR);
}

[shader("raygeneration")]
void ProbeCacheTraceGen()
{
    const uint i = DispatchRaysIndex().x;
    const uint probe = i / 64, lane = i % 64;
    const uint2 coord = uint2(probe % lgProbeViewSize().x, probe / lgProbeViewSize().x);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> depths = ResourceDescriptorHeap[P[10].w];
    if (probe >= lgProbeCount(adaptive) || !(depths[coord] > 0)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[P[11].y];
    const float3 position = positions[coord].xyz;
    const uint paramsSrv = lgFrame() % 8 == 7 ? 0xFFFFFFFFu : P[0].x;
    const uint depthSrv = lgFrame() & 1 ? P[1].x : 0xFFFFFFFFu;
    RayDesc r;
    r.Origin = position;
    r.Direction = lgSphere((float2(lane % 8, lane / 8) + 0.5) / 8.0);
    r.TMin = i % 7 == 0 ? 2.0 : 0.0;
    r.TMax = i % 11 == 0 ? 2.5 : 20.0;
    RayDesc referenceRay = r;
    LrcCoverage expectedCoverage;
    float4 expectedCache;
    const RtHit expected = referenceTrace(rtScene(), referenceRay, paramsSrv, P[0].y, P[0].w, depthSrv,
                                          position, lgRcDither(coord), expectedCoverage, expectedCache);
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].y];
    stats.InterlockedAdd(16, 1);
    if (expected.t >= 0) stats.InterlockedAdd(20, 1);
    else if (expectedCoverage.valid) stats.InterlockedAdd(24, 1);
    else stats.InterlockedAdd(28, 1);
    for (uint mode = 1; mode <= 3; ++mode)
    {
        const bool prepared = (mode & 2) != 0;
        RayDesc ray = r;
        LrcCoverage coverage;
        float4 cached;
        const RtHit hit = lgTraceCache(rtScene(), ray, paramsSrv, prepared ? P[0].z : P[0].y, P[0].w, depthSrv,
                                       probe, position, lgRcDither(coord), prepared, (mode & 1) != 0, coverage, cached);
        bool different = asuint(hit.t) != asuint(expected.t);
        if (hit.t >= 0 && expected.t >= 0)
            different = different || hit.instance != expected.instance || hit.geometry != expected.geometry || hit.primitive != expected.primitive ||
                        hit.frontFace != expected.frontFace || any(asuint(hit.barycentrics) != asuint(expected.barycentrics));
        if (hit.t < 0 && expected.t < 0)
        {
            different = different || coverage.valid != expectedCoverage.valid || asuint(ray.TMax) != asuint(referenceRay.TMax);
            if (coverage.valid) different = different || any(asuint(cached) != asuint(expectedCache));
        }
        if (different) stats.InterlockedAdd((mode - 1) * 4, 1);
        // A successful near hit must skip the nonzero directional cache value.
        if ((mode & 1) && hit.t >= 0 && expectedCache.a > 0 && all(cached == 0)) stats.InterlockedAdd(32, 1);
    }
}
