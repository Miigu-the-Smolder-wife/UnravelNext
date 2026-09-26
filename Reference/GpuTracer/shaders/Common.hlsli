// The reference estimator on the GPU: Reference/PathTracer/src/PathTracer.cpp (PathTracer::Impl) with the shared
// sampler, material, atmosphere and light code. Structure, sample-dimension order, MIS weights, forced in-scattering,
// Russian roulette, order windows and the sun-caustic path-space partition are the CPU's line for line.
// Code size: HLSL inlines every call, so each heavy piece (traversal with alpha test, table lookups, quadrature) must
// appear at one call site. Next-event visibility is therefore queued and resolved at one place per path iteration, and
// the two medium points of a segment (forced NEE point, tracked collision) share one sampling site.
#ifndef UNX_RT_COMMON_HLSLI
#define UNX_RT_COMMON_HLSLI
#include "Scene.hlsli"

static const float kRtForceScale = 16.0f;
static const uint kRtMaxBounces = 4096;
static const uint kRtPrevCamera = 0;
static const uint kRtPrevSurface = 1;
static const uint kRtPrevMedium = 2;

float3 rtCameraRay(RtCamera c, uint w, uint h, float px, float py)
{
    const float nx = px / (float)w * 2 - 1, ny = 1 - py / (float)h * 2;
    return normalize(c.forward + c.right * (nx * c.tanHalfFov * c.aspect) + c.up * (ny * c.tanHalfFov));
}

float rtOrderWeight(RtConstants C, uint kVol, uint kSurf)
{
    return kVol >= C.orderMin && kVol <= C.orderMax && kSurf >= C.surfMin && kSurf <= C.surfMax ? 1.0f : 0.0f;
}

// Sun radiance arriving at p from direction w (in the cone): planet, atmosphere and scene visibility.
float3 rtSunArriving(RtConstants C, float3 p, float3 w, float3 offsetNormal, bool useOffset)
{
    if (rtAtmGroundDistance(C.atm, p, w) > 0) return float3(0, 0, 0);
    const float3 tau = rtAtmDepthToTop(C.atm, p, w);
    if (!rtFinite3(tau)) return float3(0, 0, 0);
    const float3 o = useOffset ? rtOffsetRayOrigin(p, offsetNormal) : p;
    if (rtOccluded(o, w, 0.0f, kRtFarT)) return float3(0, 0, 0);
    return C.sun.radiance * rtExpNeg3(tau);
}

// Next-event queries of one path iteration. Their contributions are additive and their visibility and transmittance
// consume no random numbers, so they are queued where the CPU evaluates them (the sample-dimension order stays the
// CPU's) and resolved at one place.
struct RtQuery
{
    float3 p;       // point the transmittance starts from
    float3 o;       // shadow-ray origin (offset or p)
    float3 dir;
    float tfar;     // shadow-ray length
    float dist;     // light: distance for the transmittance
    float3 weight;  // contribution when visible, before the transmittance
    uint kind;      // 0 sun (ground test, depth to the top), 1 light without shadow, 2 shadowed light
};
static const uint kRtMaxQueries = 4;
struct RtQueries
{
    RtQuery q[kRtMaxQueries];
    uint n;
};

void rtQueueSun(inout RtQueries qs, RtConstants C, float3 p, float3 w, float3 offsetNormal, bool useOffset, float3 weight)
{
    RtQuery q;
    q.p = p;
    q.o = useOffset ? rtOffsetRayOrigin(p, offsetNormal) : p;
    q.dir = w;
    q.tfar = kRtFarT;
    q.dist = 0;
    q.weight = weight * C.sun.radiance;
    q.kind = 0;
    qs.q[qs.n] = q;
    qs.n += 1;
}
void rtQueueLight(inout RtQueries qs, RtLight l, float3 p, float3 offsetNormal, bool useOffset, RtLightSample ls, float3 weight)
{
    RtQuery q;
    q.p = p;
    q.o = useOffset ? rtOffsetRayOrigin(p, offsetNormal) : p;
    q.dir = ls.wi;
    q.tfar = ls.distance * (1 - 1e-4f);
    q.dist = ls.distance;
    q.weight = weight * ls.L;
    q.kind = l.castShadow != 0 ? 2u : 1u;
    qs.q[qs.n] = q;
    qs.n += 1;
}
float3 rtResolveQueries(RtConstants C, inout RtQueries qs)
{
    float3 sum = float3(0, 0, 0);
    [loop] for (uint i = 0; i < qs.n; ++i)
    {
        const RtQuery q = qs.q[i];
        bool visible = true;
        float3 T = float3(0, 0, 0);
        if (q.kind == 0)
        {
            if (rtAtmGroundDistance(C.atm, q.p, q.dir) > 0) visible = false;
            else
            {
                const float3 tau = rtAtmDepthToTop(C.atm, q.p, q.dir);
                if (!rtFinite3(tau)) visible = false;
                else T = rtExpNeg3(tau);
            }
        }
        if (visible && q.kind != 1) visible = !rtOccluded(q.o, q.dir, 0.0f, q.tfar);
        if (visible && q.kind != 0) T = rtExpNeg3(rtAtmOpticalDepth(C.atm, q.p, q.dir, q.dist));
        if (visible) sum += q.weight * T;
    }
    qs.n = 0;
    return sum;
}

// Direct light (sun + one local light) scattered at y towards -d (Impl::mediumNee), queued with the factor 'scale'.
void rtMediumNee(RtConstants C, float3 y, RtAtmCoefficients c, float3 d, inout RtSampler smp, float3 scale, inout RtQueries qs)
{
    float u1, u2;
    rtGet2D(smp, u1, u2);
    const float3 ws = rtSampleSun(C.sun, u1, u2);
    const float cs = dot(ws, d);
    const float3 phaseS = c.scatteringRayleigh * rtPhaseRayleigh(cs) + c.scatteringMie * rtPhaseMie(C.atm, cs);
    if (!rtIsZero3(phaseS)) rtQueueSun(qs, C, y, ws, float3(0, 0, 0), false, scale * phaseS * C.sun.solidAngle);
    const float uSel = rtGet1D(smp);
    rtGet2D(smp, u1, u2);
    if (C.grid.count > 0)
    {
        const uint cell = rtLightCell(C.grid, y);
        const float total = rtLightTotal(cell, y);
        if (total > 0)
        {
            float pSel;
            const uint li = rtLightChoose(cell, y, total, uSel, pSel);
            const RtLight l = rtLightFetch(li);
            RtLightSample ls;
            if (rtLightSample(l, y, u1, u2, ls))
            {
                const float cl = dot(ls.wi, d);
                const float3 phaseL = c.scatteringRayleigh * rtPhaseRayleigh(cl) + c.scatteringMie * rtPhaseMie(C.atm, cl);
                rtQueueLight(qs, l, y, float3(0, 0, 0), false, ls, scale * phaseL * (1.0f / (pSel * ls.pdf)));
            }
        }
    }
}

float3 rtRadiance(RtConstants C, float3 origin, float3 dir, float tnear, inout RtSampler smp, out bool truncatedPath)
{
    truncatedPath = false;
    float3 L = float3(0, 0, 0), beta = float3(1, 1, 1);
    float3 o = origin, d = dir;
    float tmin = tnear;
    uint prev = kRtPrevCamera;
    float prevBsdfPdf = 0;
    float3 prevPos = float3(0, 0, 0);
    uint prevCell = ~0u;
    float prevTotal = 0;
    uint nVol = 0, surfVerts = 0;
    bool chain = false, dropSun = false;
    const bool forced = C.forced != 0, caustics = C.caustics != 0;
    RtQueries qs;
    qs.n = 0;
    bool alive = true;

    // Every exit of the CPU loop ("break") is "alive = false; continue": the queued queries of the iteration are
    // resolved at the top of the loop, which then ends.
    [loop] for (uint bounce = 0;; ++bounce)
    {
        if (qs.n > 0) L += rtResolveQueries(C, qs);
        if (!alive) break;
        if (bounce >= kRtMaxBounces)
        {
            truncatedPath = true;
            break;
        }
        // --- trace the segment
        RtHit hit;
        const bool surf = rtIntersect(o, d, tmin, kRtFarT, kRtMaskAll, hit);
        uint end;  // 0 surface, 1 ground, 2 space
        float segLen;
        if (surf)
        {
            end = 0;
            segLen = hit.t;
        }
        else
        {
            const float g = rtAtmGroundDistance(C.atm, o, d);
            if (g > 0)
            {
                end = 1;
                segLen = g;
            }
            else
            {
                // Below the planet surface heading further down with no scene geometry: absorbed by the ground.
                if (rtAtmQ(C.atm, o) < 0 && rtAtmRadialDot(C.atm, o, d) < 0)
                {
                    alive = false;
                    continue;
                }
                end = 2;
                segLen = rtAtmTopDistance(C.atm, o, d);
                if (!(segLen > 0)) segLen = 0;
            }
        }

        // --- 1. analytic area-light emission along the segment (after a surface vertex)
        if (prev == kRtPrevSurface && prevCell != ~0u && prevTotal > 0)
        {
            float cum = 0;
            const uint k1 = rtLightCellStart(prevCell + 1);
            for (uint k = rtLightCellStart(prevCell); k < k1; ++k)
            {
                const uint li = rtLightCellLight(k);
                const RtLight l = rtLightFetch(li);
                const float imp = rtLightImportance(l, prevPos);
                if (imp <= 0) continue;
                const float prevCum = cum;
                cum += imp;
                if (l.castShadow == 0) continue;  // unshadowed lights are estimated by NEE alone
                float t, pdfSA;
                float3 Le;
                if (!rtLightIntersect(l, prevPos, d, segLen, t, Le, pdfSA)) continue;
                const float pSel = (cum - prevCum) / prevTotal;
                const float w = rtPowerHeuristic(prevBsdfPdf, pSel * pdfSA);
                L += beta * rtExpNeg3(rtAtmOpticalDepth(C.atm, o, d, t)) * Le * (w * rtOrderWeight(C, nVol, surfVerts));
            }
        }

        // --- 2./3. atmosphere on the segment
        float3 tauSeg = end == 2 ? rtAtmDepthToTop(C.atm, o, d) : rtAtmOpticalDepth(C.atm, o, d, segLen);
        if (!rtFinite3(tauSeg)) tauSeg = rtAtmOpticalDepth(C.atm, o, d, segLen);
        const float3 Tseg = rtExpNeg3(tauSeg);
        const float Tavg = rtAvg3(Tseg);
        const RtSegmentPdf spdf = rtSegmentPdf(C.atm, o, d, segLen);
        if (segLen > 0)
        {
            const float pForce = forced ? min(1.0f, kRtForceScale * (1 - Tavg)) : 0.0f;
            bool doForce = false;
            if (pForce > 0) doForce = rtGet1D(smp) < pForce;
            // Medium points on the segment: phase 0 the forced NEE point (2.), phase 1 the tracked collision (3.).
            const float pScatter = 1 - Tavg;
            bool doScatter = false;
            float3 y = float3(0, 0, 0), T = float3(0, 0, 0);
            RtAtmCoefficients c;
            c.scatteringRayleigh = c.scatteringMie = c.extinction = float3(0, 0, 0);
            float pt = 1;
            [loop] for (uint phase = 0; phase < 2; ++phase)
            {
                if (phase == 0 && !doForce)
                {
                    if (forced)
                    {
                        float a, b;
                        rtGet1D(smp);
                        rtGet2D(smp, a, b);
                        rtGet1D(smp);
                        rtGet2D(smp, a, b);
                    }
                    continue;
                }
                if (phase == 1)
                {
                    if (pScatter > 0) doScatter = rtGet1D(smp) < pScatter;
                    if (!doScatter) break;
                }
                const float t = rtSegmentSample(spdf, rtGet1D(smp), pt);
                y = o + d * t;
                T = rtExpNeg3(rtAtmOpticalDepth(C.atm, o, d, t));
                c = rtAtmAt(C.atm, rtAtmAltitude(C.atm, y));
                if (phase == 0) rtMediumNee(C, y, c, d, smp, beta * T * (rtOrderWeight(C, nVol + 1, surfVerts) / (pt * pForce)), qs);
            }
            if (doScatter)
            {
                float u1, u2;
                rtGet2D(smp, u1, u2);
                const float uMix = rtGet1D(smp);
                if (rtMax3(c.scatteringRayleigh) + rtMax3(c.scatteringMie) <= 0)  // absorption-only point: the path ends
                {
                    alive = false;
                    continue;
                }
                if (!forced) rtMediumNee(C, y, c, d, smp, beta * T * (rtOrderWeight(C, nVol + 1, surfVerts) / (pt * pScatter)), qs);
                float pdfDir;
                const float3 w = rtSamplePhase(C.atm, d, rtAvg3(c.scatteringRayleigh), rtAvg3(c.scatteringMie), uMix, u1, u2, pdfDir);
                const float cw = dot(w, d);
                const float3 phaseV = c.scatteringRayleigh * rtPhaseRayleigh(cw) + c.scatteringMie * rtPhaseMie(C.atm, cw);
                beta *= T * phaseV * (1.0f / (pt * pScatter * pdfDir));
                o = y;
                d = w;
                tmin = 0;
                prev = kRtPrevMedium;
                chain = false;
                dropSun = false;
                if (++nVol > C.orderMax)
                {
                    alive = false;
                    continue;
                }
                if (bounce + 1 >= C.rrStart)
                {
                    const float q = min(1.0f, rtMax3(beta));
                    if (!(rtGet1D(smp) < q))
                    {
                        alive = false;
                        continue;
                    }
                    beta *= 1.0f / q;
                }
                continue;
            }
            beta *= Tseg * (1.0f / max(Tavg, 1e-30f));
        }

        // --- segment end
        if (end == 2)
        {
            if (prev != kRtPrevMedium && !(prev == kRtPrevSurface && dropSun) && rtInSun(C.sun, d))
            {
                const float w = prev == kRtPrevCamera ? 1.0f : rtPowerHeuristic(prevBsdfPdf, 1.0f / C.sun.solidAngle);
                L += beta * C.sun.radiance * (w * rtOrderWeight(C, nVol, surfVerts));
            }
            alive = false;
            continue;
        }
        RtSurface s;
        bool lambert = false;
        if (end == 0)
        {
            s = rtSurfaceAt(hit, d);
            if (!rtIsZero3(s.emission)) L += beta * s.emission * rtOrderWeight(C, nVol, surfVerts);
            if (!s.frontFacing)
            {
                alive = false;
                continue;
            }
        }
        else
        {
            s = rtGroundSurface(C.atm, o + d * segLen);
            lambert = true;
        }
        const float3 wo = -d;
        const RtBsdf bsdf = rtBsdfInit(s, wo, lambert);
        {
            const bool isSmooth = !lambert && rtSmooth(s);
            ++surfVerts;
            if (surfVerts == 1) chain = prev == kRtPrevCamera && !isSmooth;
            else if (chain) chain = isSmooth;
            // Sun seen from this vertex belongs to the light tracer when the chain holds on a rigid smooth vertex.
            dropSun = caustics && chain && surfVerts >= 2 && isSmooth && end == 0 && !rtDeformed(hit.instance);
            if (surfVerts > C.surfMax)
            {
                alive = false;
                continue;
            }
        }

        // Sun NEE.
        {
            float u1, u2;
            rtGet2D(smp, u1, u2);
            const float3 ws = rtSampleSun(C.sun, u1, u2);
            const float3 f = rtBsdfEval(bsdf, ws);
            if (!rtIsZero3(f) && !dropSun)
            {
                const float3 side = dot(s.ng, ws) >= 0 ? s.ng : -s.ng;
                const float pl = 1.0f / C.sun.solidAngle;
                rtQueueSun(qs, C, s.p, ws, side, true,
                           beta * f * (rtBsdfCosine(bsdf, ws) * rtPowerHeuristic(pl, rtBsdfPdf(bsdf, ws)) * rtOrderWeight(C, nVol, surfVerts) / pl));
            }
        }
        // Local light NEE.
        const uint cell = rtLightCell(C.grid, s.p);
        const float total = rtLightTotal(cell, s.p);
        {
            const float uSel = rtGet1D(smp);
            float u1, u2;
            rtGet2D(smp, u1, u2);
            if (total > 0)
            {
                float pSel;
                const uint li = rtLightChoose(cell, s.p, total, uSel, pSel);
                const RtLight l = rtLightFetch(li);
                RtLightSample ls;
                if (rtLightSample(l, s.p, u1, u2, ls))
                {
                    const float3 f = rtBsdfEval(bsdf, ls.wi);
                    if (!rtIsZero3(f))
                    {
                        const float3 side = dot(s.ng, ls.wi) >= 0 ? s.ng : -s.ng;
                        const float pl = pSel * ls.pdf;
                        const bool mis = !ls.delta && l.castShadow != 0;
                        const float w = mis ? rtPowerHeuristic(pl, rtBsdfPdf(bsdf, ls.wi)) : 1.0f;
                        rtQueueLight(qs, l, s.p, side, true, ls, beta * f * (rtBsdfCosine(bsdf, ls.wi) * w * rtOrderWeight(C, nVol, surfVerts) / pl));
                    }
                }
            }
        }
        // Continue.
        const float uLobe = rtGet1D(smp);
        float u1, u2;
        rtGet2D(smp, u1, u2);
        float3 wi, bf;
        float bpdf;
        if (!rtBsdfSample(bsdf, uLobe, u1, u2, wi, bf, bpdf))
        {
            alive = false;
            continue;
        }
        beta *= bf * (rtBsdfCosine(bsdf, wi) / bpdf);
        prevBsdfPdf = bpdf;
        prevPos = s.p;
        prevCell = cell;
        prevTotal = total;
        prev = kRtPrevSurface;
        o = rtOffsetRayOrigin(s.p, dot(s.ng, wi) >= 0 ? s.ng : -s.ng);
        d = wi;
        tmin = 0;
        if (bounce + 1 >= C.rrStart)
        {
            const float q = min(1.0f, rtMax3(beta));
            if (!(rtGet1D(smp) < q))
            {
                alive = false;
                continue;
            }
            beta *= 1.0f / q;
        }
        if (rtIsZero3(beta)) alive = false;
    }
    return L;
}

// Adds v to the pixel's double3 in a half's accumulator (24 bytes per pixel). One thread owns a (pixel, half) per
// dispatch, so no atomics are needed.
void rtAccumulateDouble3(RWByteAddressBuffer accum, uint pixel, float3 v)
{
    const uint addr = pixel * 24;
    const uint4 ab = accum.Load4(addr);
    const uint2 c = accum.Load2(addr + 16);
    double x = asdouble(ab.x, ab.y), y = asdouble(ab.z, ab.w), z = asdouble(c.x, c.y);
    x += (double)v.x;
    y += (double)v.y;
    z += (double)v.z;
    uint4 o;
    uint2 o2;
    asuint(x, o.x, o.y);
    asuint(y, o.z, o.w);
    asuint(z, o2.x, o2.y);
    accum.Store4(addr, o);
    accum.Store2(addr + 16, o2);
}

static const uint kRtMaxCasRetries = 1024;

// Atomic float add by compare-exchange on the bits (bounded retries, INTERFACES 3.6).
void rtAtomicAddFloat(RWByteAddressBuffer b, uint addr, float v)
{
    uint expected = b.Load(addr);
    for (uint k = 0; k < kRtMaxCasRetries; ++k)
    {
        uint original;
        b.InterlockedCompareExchange(addr, expected, asuint(asfloat(expected) + v), original);
        if (original == expected) return;
        expected = original;
    }
    g_errors |= kRtErrorSplatCas;
}
#endif
