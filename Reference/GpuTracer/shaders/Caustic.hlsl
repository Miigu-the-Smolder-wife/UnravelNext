// unx-kernel: cs_6_6 main
// Sun-caustic light paths (PathTracer::Impl::traceCaustic): sun -> smooth rigid vertices -> first rough vertex x,
// connected to the pinhole camera and splatted with weight 1 / (W H). One thread per light path; W H paths per sample
// pass per half, split into dispatches [pathBase, pathBase + pathCount). Splats go to the half's float3 splat buffer by
// atomic float add; Resolve.hlsl adds that buffer into the double accumulator after every pass. Each path has its own
// PCG32 stream seeded from (seed, half, pass, path) (the CPU seeds one stream per 4096 consecutive paths).
#include "Common.hlsli"

uint rtEmitSearch(RtConstants C, double target)
{
    // First triangle whose running area exceeds target (std::upper_bound), clamped to the last one.
    ByteAddressBuffer cdf = ResourceDescriptorHeap[C.b.emitCdf];
    uint lo = 0, hi = C.emitCount;
    for (uint k = 0; k < 64 && lo < hi; ++k)
    {
        const uint mid = (lo + hi) >> 1;
        const uint2 bits = cdf.Load2(mid * 8);
        if (asdouble(bits.x, bits.y) > target) hi = mid;
        else lo = mid + 1;
        if (k == 63 && lo < hi) g_errors |= kRtErrorEmitSearch;
    }
    return min(lo, C.emitCount - 1);
}

void rtTraceCaustic(RtConstants C, uint half_, inout RtPcg32 rng)
{
    const double emitArea = asdouble(C.emitAreaLo, C.emitAreaHi);
    const double ua = (double)rtPcgUniform(rng) * emitArea;
    StructuredBuffer<RtEmitTriangle> tris = ResourceDescriptorHeap[C.b.emitTriangles];
    const RtEmitTriangle et = tris[rtEmitSearch(C, ua)];
    const float r1 = rtPcgUniform(rng), r2 = rtPcgUniform(rng), su = sqrt(r1);
    // The emitting point as a hit on (instance, BLAS geometry = submesh, primitive); the geometry comes from the CPU.
    const RtInstance in_ = rtInstance(et.instance);
    StructuredBuffer<RtMeshSubmeshes> ms = ResourceDescriptorHeap[C.b.meshSubmeshes];
    StructuredBuffer<RtSubmesh> sm = ResourceDescriptorHeap[C.b.submeshes];
    const uint geometry = et.geometry;
    RtHit h;
    h.instance = et.instance;
    h.geometry = geometry;
    h.primitive = et.triIndex - sm[ms[in_.sourceMesh].first + geometry].firstTriangle;
    h.u = 1 - su;
    h.v = r2 * su;
    h.t = 0;
    const float u1 = rtPcgUniform(rng), u2 = rtPcgUniform(rng);
    if (!rtAlphaOpaque(h.instance, h.geometry, h.primitive, h.u, h.v)) return;
    const float3 ws = rtSampleSun(C.sun, u1, u2);
    RtSurface s = rtSurfaceAt(h, -ws);
    if (!s.frontFacing || !rtSmooth(s)) return;
    const float nsl = dot(s.ns, ws);
    if (dot(s.ng, ws) <= 0 || nsl <= 0) return;
    const float3 Ls = rtSunArriving(C, s.p, ws, s.ng, s.extent, true);
    if (rtIsZero3(Ls)) return;
    float3 beta = Ls * (float)(emitArea * (double)C.sun.solidAngle * (double)nsl);
    float3 l = ws;
    for (uint depth = 0; depth < 16; ++depth)
    {
        // At smooth vertex s: sample the camera-side direction v; weight f(v, l) |l.ns| |v.ng| / (|l.ng| pdf(v)).
        const RtBsdf bl = rtBsdfInit(s, l, false);
        const float ul = rtPcgUniform(rng), ua1 = rtPcgUniform(rng), ua2 = rtPcgUniform(rng);
        float3 v, fs;
        float pdfv;
        if (!rtBsdfSample(bl, ul, ua1, ua2, v, fs, pdfv)) return;
        const float gv = dot(s.ng, v);
        if (gv <= 0 || dot(s.ns, v) <= 0) return;
        const float3 f = rtBsdfEval(rtBsdfInit(s, v, false), l);
        if (rtIsZero3(f)) return;
        beta *= f * (abs(dot(l, s.ns)) / abs(dot(l, s.ng)) * gv / pdfv);
        // Next vertex (scene surface or planet ground); transmittance along the segment (no medium event here: that
        // path class belongs to the camera paths).
        const float3 o = rtOffsetRayOrigin(s.p, s.ng, s.extent);
        RtHit hn;
        RtSurface x;
        bool lambert = false;
        float len;
        const bool surf = rtIntersect(o, v, 0.0f, kRtFarT, kRtMaskAll, hn);
        if (surf)
        {
            len = hn.t;
            x = rtSurfaceAt(hn, v);
            if (!x.frontFacing) return;
        }
        else
        {
            const float g = rtAtmGroundDistance(C.atm, o, v);
            if (!(g > 0)) return;
            len = g;
            x = rtGroundSurface(C.atm, o + v * g);
            lambert = true;
        }
        beta *= rtExpNeg3(rtAtmOpticalDepth(C.atm, o, v, len));
        l = -v;
        if (!lambert && rtSmooth(x))
        {
            s = x;
            continue;
        }
        // First rough vertex x: connect to the pinhole camera, or to a uniform point o of the thin lens (the pixel
        // measure at a fixed lens point is the pinhole's at o; the pixel is where the ray o -> x meets the plane of
        // focus: u = (d.right) / (d.forward) + o_right / focus).
        float3 camO = C.camera.position;
        float2 lens = float2(0, 0);
        if (C.camera.lensRadius > 0)
        {
            const float u1 = rtPcgUniform(rng), u2 = rtPcgUniform(rng);
            lens = rtConcentricDisk(u1, u2) * C.camera.lensRadius;
            camO += C.camera.right * lens.x + C.camera.up * lens.y;
        }
        const float3 toCam = camO - x.p;
        const float d2 = dot(toCam, toCam), dist = sqrt(d2);
        const float3 wc = toCam * (1.0f / dist), dc = -wc;
        const float zc = dot(dc, C.camera.forward);
        if (zc <= 1e-6f) return;
        const float th = C.camera.tanHalfFov, aspect = C.camera.aspect;
        const float shiftX = C.camera.lensRadius > 0 ? lens.x / C.camera.focusDistance : 0.0f;
        const float shiftY = C.camera.lensRadius > 0 ? lens.y / C.camera.focusDistance : 0.0f;
        const float nx = (dot(dc, C.camera.right) / zc + shiftX) / (th * aspect), ny = (dot(dc, C.camera.up) / zc + shiftY) / th;
        const float px = (nx + 1) * 0.5f * (float)C.width, py = (1 - ny) * 0.5f * (float)C.height;
        if (!(px >= 0 && px < (float)C.width && py >= 0 && py < (float)C.height)) return;
        const float tn = C.camera.nearPlane / zc;
        if (dist <= tn) return;
        RtSurface xc = x;
        if (surf)
        {
            xc = rtSurfaceAt(hn, dc);
            if (!xc.frontFacing) return;
        }
        else if (dot(xc.ng, wc) <= 0) return;
        const float gc = abs(dot(wc, xc.ng));
        const float3 fx = rtBsdfEval(rtBsdfInit(xc, wc, lambert), l);
        if (rtIsZero3(fx)) return;
        RtHit hv;
        if (rtIntersect(camO, dc, tn, dist * (1 - 1e-4f), kRtMaskAll, hv)) return;
        if (!surf && rtAtmGroundDistance(C.atm, camO, dc) < dist * (1 - 1e-4f)) return;
        const float3 T = rtExpNeg3(rtAtmOpticalDepth(C.atm, camO, dc, dist));
        const float Ap = (2 * th * aspect / (float)C.width) * (2 * th / (float)C.height);
        const float scale = 1.0f / ((float)C.width * (float)C.height);
        const float3 val = beta * fx * T * (abs(dot(l, xc.ns)) / abs(dot(l, xc.ng)) * gc / (d2 * Ap * zc * zc * zc) * rtOrderWeight(C, 0, depth + 2) * scale);
        if (!rtFinite3(val)) return;
        const uint pi = min((uint)py, C.height - 1) * C.width + min((uint)px, C.width - 1);
        RWByteAddressBuffer splat = ResourceDescriptorHeap[half_ == 0 ? C.b.splat0 : C.b.splat1];
        rtAtomicAddFloat(splat, pi * 12, val.x);
        rtAtomicAddFloat(splat, pi * 12 + 4, val.y);
        rtAtomicAddFloat(splat, pi * 12 + 8, val.z);
        return;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    if (id.x < g_root.pathCount)
    {
        const uint half_ = id.z + g_root.halfBase, path = g_root.pathBase + id.x;
        const uint seed = rtHashCombine(rtHashCombine(rtHashCombine(rtHashCombine(C.seedLo ^ C.seedHi ^ 0xCA057105u, half_), g_root.passIndex), path >> 16), path & 0xFFFFu);
        RtPcg32 rng = rtPcgInit((uint64_t)seed, 0xC0FFEEull + (uint64_t)half_);
        rtTraceCaustic(C, half_, rng);
    }
    rtFlushCounters(0, 0, 0);
}
