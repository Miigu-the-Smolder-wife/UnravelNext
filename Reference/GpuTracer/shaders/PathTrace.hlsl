// unx-kernel: cs_6_6 main
// Camera paths of the reference estimator (Reference/PathTracer/src/PathTracer.cpp, PathTracer::Impl::radiance) as a
// wavefront (2026-09-26: a dispatch's worst time is bounded by its structure, not by the paths' lengths). One slot per
// (pixel, half) of the dispatch rectangle runs samples [c0, c1) of that half in turn. A dispatch advances every listed
// slot by at most `budget` path iterations (Common.hlsli rtPathStep: one segment with its events and the next vertex),
// starting the slot's next sample in place when one ends; a slot with work left is appended to the output list (compacted)
// for the next dispatch (indirect, PathArgs.hlsl). When its last sample ends a slot adds the float sum of its samples to
// the half's double accumulator: the per-thread float sum of the non-wavefront kernel, so the image is the same.
//
// Worst dispatch = listed slots x budget x the worst path iteration, whatever the paths: slots <= the rectangle's
// (GpuPathTracer.cpp kMaxSlots); an iteration is one intersection (candidates capped by kRtMaxCandidates), the
// segment's atmosphere quadrature (at most 4096 panels of 8 points, 2 km panels over the altitude span otherwise, or
// table lookups), the area lights of one light-grid cell, and at most kRtMaxQueries next-event queries.
//
// Root: constants, x0 = wave buffer (RWByteAddressBuffer: [0] count of list 0, [4] count of list 1, [16..44] the
// rectangle: x0, y0, width, height, halves, first half, c0, c1), y0 = path states (RWByteAddressBuffer, kStateBytes per
// slot), w = list 0, h = list 1 (RWByteAddressBuffer, slot indices), sampleBegin = mode (0: start - thread = slot, every
// slot -> list 1; 1: list 0 -> list 1; 2: list 1 -> list 0), sampleEnd = budget. The output list's count must be 0.
// Mode 3 (GpuPathTracer::calibrate, on this kernel's pipeline so with its occupancy): fixed work per thread - an
// atmosphere quadrature of sampleEnd panels (8 points each) and pathBase alpha tests of (instance w, geometry h, primitive
// passIndex) - its sums to y0 (RWByteAddressBuffer, 16 bytes per thread): the per-lane cost of a quadrature point and of
// an alpha candidate, and from the time of many threads against one group, the lanes the GPU runs at once.
#include "Common.hlsli"

static const uint kStateBytes = 140;

void storePath(RWByteAddressBuffer b, uint slot, RtPath p, RtSampler smp, uint si, float3 acc)
{
    const uint a = slot * kStateBytes;
    b.Store3(a + 0, asuint(p.L));
    b.Store3(a + 12, asuint(p.beta));
    b.Store3(a + 24, asuint(p.o));
    b.Store3(a + 36, asuint(p.d));
    b.Store4(a + 48, uint4(asuint(p.tmin), asuint(p.prevBsdfPdf), asuint(p.prevTotal), p.prevCell));
    b.Store3(a + 64, asuint(p.prevPos));
    const uint flags = (p.chain ? 1u : 0u) | (p.dropSun ? 2u : 0u) | (p.alive ? 4u : 0u) | (p.truncated ? 8u : 0u);
    b.Store4(a + 76, uint4(p.prev, p.nVol, p.surfVerts, p.bounce));
    b.Store2(a + 92, uint2(flags, si));
    b.Store3(a + 100, asuint(acc));
    b.Store3(a + 112, uint3(smp.seed, smp.index, smp.pair));
    b.Store4(a + 124, uint4((uint)smp.rng.state, (uint)(smp.rng.state >> 32), (uint)smp.rng.inc, (uint)(smp.rng.inc >> 32)));
}

void loadPath(RWByteAddressBuffer b, uint slot, out RtPath p, out RtSampler smp, out uint si, out float3 acc)
{
    const uint a = slot * kStateBytes;
    p.L = asfloat(b.Load3(a + 0));
    p.beta = asfloat(b.Load3(a + 12));
    p.o = asfloat(b.Load3(a + 24));
    p.d = asfloat(b.Load3(a + 36));
    const uint4 w = b.Load4(a + 48);
    p.tmin = asfloat(w.x);
    p.prevBsdfPdf = asfloat(w.y);
    p.prevTotal = asfloat(w.z);
    p.prevCell = w.w;
    p.prevPos = asfloat(b.Load3(a + 64));
    const uint4 c = b.Load4(a + 76);
    p.prev = c.x;
    p.nVol = c.y;
    p.surfVerts = c.z;
    p.bounce = c.w;
    const uint2 f = b.Load2(a + 92);
    p.chain = (f.x & 1u) != 0;
    p.dropSun = (f.x & 2u) != 0;
    p.alive = (f.x & 4u) != 0;
    p.truncated = (f.x & 8u) != 0;
    si = f.y;
    acc = asfloat(b.Load3(a + 100));
    const uint3 s = b.Load3(a + 112);
    smp.seed = s.x;
    smp.index = s.y;
    smp.pair = s.z;
    const uint4 r = b.Load4(a + 124);
    smp.rng.state = ((uint64_t)r.y << 32) | (uint64_t)r.x;
    smp.rng.inc = ((uint64_t)r.w << 32) | (uint64_t)r.z;
}

// Sample si of the pixel: the camera ray (the non-wavefront kernel's order of sample dimensions) and a new path.
RtPath startSample(RtConstants C, uint x, uint y, uint pixelSeed, uint si, out RtSampler smp)
{
    smp = rtSamplerInit(pixelSeed, si);
    float jx, jy;
    rtGet2D(smp, jx, jy);
    float3 dir = rtCameraRay(C.camera, C.width, C.height, (float)x + jx, (float)y + jy);
    float3 origin = C.camera.position;
    if (C.camera.lensRadius > 0)  // thin lens: two more sample dimensions (none for the pinhole)
    {
        float lu, lv;
        rtGet2D(smp, lu, lv);
        rtLensRay(C.camera, dir, lu, lv, origin, dir);
    }
    const float tn = C.camera.nearPlane / max(dot(dir, C.camera.forward), 1e-3f);
    return rtPathStart(origin, dir, tn);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    RWByteAddressBuffer wave = ResourceDescriptorHeap[g_root.x0];
    RWByteAddressBuffer states = ResourceDescriptorHeap[g_root.y0];
    RWByteAddressBuffer list0 = ResourceDescriptorHeap[g_root.w];
    RWByteAddressBuffer list1 = ResourceDescriptorHeap[g_root.h];
    const uint mode = g_root.sampleBegin, budget = g_root.sampleEnd;
    if (mode == 3)  // calibration: uniform over the dispatch, so the early return keeps the wave operations uniform
    {
        RWByteAddressBuffer out_ = ResourceDescriptorHeap[g_root.y0];
        const float3 o = float3(0, 1 + (float)id.x * 1e-3f, 0), d = normalize(float3(1, 0.05f, 0));
        const float3 tau = rtAtmIntegrate(C.atm, o, d, 0, 50000.0f, budget);
        uint opaque = 0;
        [loop] for (uint k = 0; k < g_root.pathBase; ++k)
            opaque += rtAlphaOpaque(g_root.w, g_root.h, g_root.passIndex, frac((float)(k + id.x) * 0.6180339f), frac((float)k * 0.3819660f) * 0.5f) ? 1u : 0u;
        out_.Store4(id.x * 16, uint4(asuint(tau), opaque));
        return;
    }
    const uint4 rect = wave.Load4(16);   // x0, y0, width, height
    const uint4 rect2 = wave.Load4(32);  // halves, first half, c0, c1
    const uint pixels = rect.z * rect.w, slots = pixels * rect2.x;
    uint nans = 0, truncated = 0;
    const uint count = mode == 0 ? slots : wave.Load(mode == 1 ? 0 : 4);
    if (id.x < count)
    {
        const uint slot = mode == 0 ? id.x : (mode == 1 ? list0.Load(id.x * 4) : list1.Load(id.x * 4));
        const uint inRect = slot % pixels, half_ = rect2.y + slot / pixels;
        const uint x = rect.x + inRect % rect.z, y = rect.y + inRect / rect.z;
        const uint pi = y * C.width + x;
        const uint pixelSeed = rtHashCombine(rtHashCombine(C.seedLo ^ C.seedHi, half_), pi);
        RtPath p;
        RtSampler smp;
        uint si;
        float3 acc;
        if (mode == 0)
        {
            si = rect2.z;
            acc = float3(0, 0, 0);
            p = startSample(C, x, y, pixelSeed, si, smp);
        }
        else loadPath(states, slot, p, smp, si, acc);
        bool finished = false;
        [loop] for (uint k = 0; k < budget; ++k)
        {
            if (rtPathStep(C, p, smp)) continue;
            // the sample has ended: its value into the slot's float sum, then the slot's next sample
            truncated += p.truncated ? 1 : 0;
            if (rtFinite3(p.L)) acc += p.L;
            else ++nans;
            if (++si >= rect2.w)
            {
                finished = true;
                break;
            }
            p = startSample(C, x, y, pixelSeed, si, smp);
        }
        if (finished)
        {
            RWByteAddressBuffer accum = ResourceDescriptorHeap[half_ == 0 ? C.b.accum0 : C.b.accum1];
            rtAccumulateDouble3(accum, pi, acc);
        }
        else
        {
            storePath(states, slot, p, smp, si, acc);
            uint at;
            wave.InterlockedAdd(mode == 2 ? 0 : 4, 1u, at);
            if (mode == 2) list0.Store(at * 4, slot);
            else list1.Store(at * 4, slot);
        }
    }
    rtFlushCounters(nans, truncated, 0);
}
