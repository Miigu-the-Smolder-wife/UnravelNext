#ifndef UNX_LG_TRACE_CACHE_HLSLI
#define UNX_LG_TRACE_CACHE_HLSLI
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

// The near interval is needed whether or not the directional cache answers.
// Trace it before sampling the cache; only a miss needs that answer. On an
// invalid/occluded/empty answer continue the original ray to its full distance.
// Origin, direction, cone, mask and sampling identity never change. TMin of the
// caller's ray stays intact for hit shading/continuations; only the second
// traversal uses the already-examined interval's end as its TMin.
RtHit lgTraceCache(RtSceneSrvs scene, inout RayDesc ray, uint paramsSrv, uint lookupSrv, uint atlasSrv, uint depthSrv,
                   uint screenProbe, float3 position, float dither, bool prepared, bool nearFirst,
                   out LrcCoverage coverage, out float4 cached)
{
    coverage = (LrcCoverage)0;
    cached = 0;
    const uint mask = RT_MASK_GI | RT_MASK_EMITTER | RT_MASK_FAR;
    if (paramsSrv == 0xFFFFFFFFu) return rtTraceClosest(scene, ray, RAY_FLAG_NONE, mask);
    const LrcParams rc = lrcParams(paramsSrv);
    coverage = lgRcCoverageAt(rc, lookupSrv, screenProbe, position, dither, prepared);
    if (!coverage.valid) return rtTraceClosest(scene, ray, RAY_FLAG_NONE, mask);

    const float nearEnd = min(ray.TMax, coverage.minTraceDistance);
    // Empty/reversed intervals retain the reference path. In normal screen
    // continuations the pullback places TMin before the coverage boundary.
    const bool tracedNear = nearFirst && ray.TMin < nearEnd;
    RtHit nearHit = rtMiss();
    if (tracedNear)
    {
        RayDesc nearRay = ray;
        nearRay.TMax = nearEnd;
        nearHit = rtTraceClosest(scene, nearRay, RAY_FLAG_NONE, mask);
        if (nearHit.t >= 0) return nearHit;
    }

    cached = lgRcSample(rc, lookupSrv, atlasSrv, depthSrv, coverage, screenProbe, position,
                        ray.Direction, lrcSeenFrom(rc, coverage, ray.Origin, ray.Direction), prepared);
    coverage.valid = cached.a > 0;
    if (coverage.valid) ray.TMax = nearEnd;
    if (tracedNear && nearEnd >= ray.TMax) return nearHit;
    RayDesc remaining = ray;
    if (tracedNear) remaining.TMin = nearEnd;
    return rtTraceClosest(scene, remaining, RAY_FLAG_NONE, mask);
}
#endif
