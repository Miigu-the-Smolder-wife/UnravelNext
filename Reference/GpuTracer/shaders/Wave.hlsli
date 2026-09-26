// The camera-path wavefront by stage (README 3): a path iteration (Common.hlsli rtPathStep) runs as four kernels - Trace
// (section 1: the segment), Segment (section 2: area-light emission and the atmosphere on the segment), Surface (section 3:
// the segment's end and the next vertex) and Query (the iteration's next-event queries) - so each kernel holds one
// section's code and a dispatch's worst time is one section's (README 3), not a whole iteration's.
//
// A slot is one (pixel, half) of the rectangle, running samples [c0, c1) of that half in turn; its state (kWaveStateBytes)
// is the path, its sampler, the sample index, the float sum of its finished samples, the iteration's segment, its queued
// queries and the Segment stage's cursor over the cell's area lights. A slot sits in exactly one stage's list. Every stage consumes its whole list and appends each slot to the next
// stage's list: Trace -> Segment -> Surface, a section's end -> Query when it queued queries, and an iteration's end ->
// Trace (the bounce count advances there). Trace starts an iteration: a path that ended (or reached kRtMaxBounces) adds
// its sample to the slot's sum and the slot starts its next sample - or, after its last, adds the sum to the half's double
// accumulator and leaves the lists (the per-slot float sum of the non-staged kernel: the same image).
//
// Wave buffer (RWByteAddressBuffer, bytes): [0..15] per stage (Trace, Segment, Surface, Query) the append count, [16..31] the
// count of the running dispatch, [32..63] the rectangle: x0, y0, width, height, halves, first half, c0, c1.
// Root (all stage kernels): constants, x0 = wave buffer, y0 = slot states, w = the lists (RWByteAddressBuffer: 4 lists of
// maxSlots entries, list k at k x maxSlots), h = maxSlots, sampleBegin = mode (0 start, 1 run, 3 calibration).
#ifndef UNX_RT_WAVE_HLSLI
#define UNX_RT_WAVE_HLSLI
#include "Common.hlsli"

static const uint kWaveStateBytes = 460;
static const uint kWaveCandidatesPerVisit = 1024;  // alpha candidates per ray per visit (rtIntersectPart, rtOccludedPart)
static const uint kWaveLightsPerVisit = 4;  // area lights of a cell per Segment visit (rtPathEmission resumes)
static const uint kWaveTrace = 0, kWaveSegment = 1, kWaveSurface = 2, kWaveQuery = 3;

struct WaveSlot
{
    RtPath p;
    RtSampler smp;
    uint si;
    float3 acc;
    RtSegment g;
    RtQueries qs;
    uint emitK;      // rtPathEmission's cursor (kRtNone: the cell's first light)
    float emitCum;   // its importance sum so far
    float traceA, traceB;  // Trace's resumable ray: the interval of the next visit
    uint traceResume;      // 1: the segment's ray is not done (the iteration has started)
    uint queryI;           // Query's cursor: the query in progress (kRtNone: the iteration's first)
    float queryA, queryB;  // its shadow ray's interval
    float3 querySum;       // the sum of the queries done
};

struct WaveRect
{
    uint x0, y0, width, height, halves, firstHalf, c0, c1;
};

WaveRect waveRect(RWByteAddressBuffer wave)
{
    const uint4 a = wave.Load4(32), b = wave.Load4(48);
    WaveRect r;
    r.x0 = a.x;
    r.y0 = a.y;
    r.width = a.z;
    r.height = a.w;
    r.halves = b.x;
    r.firstHalf = b.y;
    r.c0 = b.z;
    r.c1 = b.w;
    return r;
}

void waveStore(RWByteAddressBuffer b, uint slot, WaveSlot w)
{
    const uint a = slot * kWaveStateBytes;
    const RtPath p = w.p;
    b.Store3(a + 0, asuint(p.L));
    b.Store3(a + 12, asuint(p.beta));
    b.Store3(a + 24, asuint(p.o));
    b.Store3(a + 36, asuint(p.d));
    b.Store4(a + 48, uint4(asuint(p.tmin), asuint(p.prevBsdfPdf), asuint(p.prevTotal), p.prevCell));
    b.Store3(a + 64, asuint(p.prevPos));
    const uint flags = (p.chain ? 1u : 0u) | (p.dropSun ? 2u : 0u) | (p.alive ? 4u : 0u) | (p.truncated ? 8u : 0u);
    b.Store4(a + 76, uint4(p.prev, p.nVol, p.surfVerts, p.bounce));
    b.Store2(a + 92, uint2(flags, w.si));
    b.Store3(a + 100, asuint(w.acc));
    b.Store3(a + 112, uint3(w.smp.seed, w.smp.index, w.smp.pair));
    b.Store4(a + 124, uint4((uint)w.smp.rng.state, (uint)(w.smp.rng.state >> 32), (uint)w.smp.rng.inc, (uint)(w.smp.rng.inc >> 32)));
    b.Store4(a + 140, uint4(w.g.hit.instance, w.g.hit.geometry, w.g.hit.primitive, asuint(w.g.hit.u)));
    b.Store4(a + 156, uint4(asuint(w.g.hit.v), asuint(w.g.hit.t), w.g.end, asuint(w.g.segLen)));
    b.Store(a + 172, w.qs.n);
    b.Store2(a + 416, uint2(w.emitK, asuint(w.emitCum)));
    b.Store4(a + 424, uint4(asuint(w.traceA), asuint(w.traceB), w.traceResume, w.queryI));
    b.Store4(a + 440, uint4(asuint(w.queryA), asuint(w.queryB), asuint(w.querySum.x), asuint(w.querySum.y)));
    b.Store(a + 456, asuint(w.querySum.z));
    [unroll] for (uint i = 0; i < kRtMaxQueries; ++i)
    {
        const uint q = a + 176 + i * 60;
        const RtQuery x = w.qs.q[i];
        b.Store4(q + 0, uint4(asuint(x.p), asuint(x.o.x)));
        b.Store4(q + 16, uint4(asuint(x.o.yz), asuint(x.dir.xy)));
        b.Store4(q + 32, uint4(asuint(x.dir.z), asuint(x.tfar), asuint(x.dist), asuint(x.weight.x)));
        b.Store3(q + 48, uint3(asuint(x.weight.yz), x.kind));
    }
}

WaveSlot waveLoad(RWByteAddressBuffer b, uint slot)
{
    const uint a = slot * kWaveStateBytes;
    WaveSlot w;
    w.p.L = asfloat(b.Load3(a + 0));
    w.p.beta = asfloat(b.Load3(a + 12));
    w.p.o = asfloat(b.Load3(a + 24));
    w.p.d = asfloat(b.Load3(a + 36));
    const uint4 x = b.Load4(a + 48);
    w.p.tmin = asfloat(x.x);
    w.p.prevBsdfPdf = asfloat(x.y);
    w.p.prevTotal = asfloat(x.z);
    w.p.prevCell = x.w;
    w.p.prevPos = asfloat(b.Load3(a + 64));
    const uint4 c = b.Load4(a + 76);
    w.p.prev = c.x;
    w.p.nVol = c.y;
    w.p.surfVerts = c.z;
    w.p.bounce = c.w;
    const uint2 f = b.Load2(a + 92);
    w.p.chain = (f.x & 1u) != 0;
    w.p.dropSun = (f.x & 2u) != 0;
    w.p.alive = (f.x & 4u) != 0;
    w.p.truncated = (f.x & 8u) != 0;
    w.si = f.y;
    w.acc = asfloat(b.Load3(a + 100));
    const uint3 s = b.Load3(a + 112);
    w.smp.seed = s.x;
    w.smp.index = s.y;
    w.smp.pair = s.z;
    const uint4 r = b.Load4(a + 124);
    w.smp.rng.state = ((uint64_t)r.y << 32) | (uint64_t)r.x;
    w.smp.rng.inc = ((uint64_t)r.w << 32) | (uint64_t)r.z;
    const uint4 h0 = b.Load4(a + 140), h1 = b.Load4(a + 156);
    w.g.hit.instance = h0.x;
    w.g.hit.geometry = h0.y;
    w.g.hit.primitive = h0.z;
    w.g.hit.u = asfloat(h0.w);
    w.g.hit.v = asfloat(h1.x);
    w.g.hit.t = asfloat(h1.y);
    w.g.end = h1.z;
    w.g.segLen = asfloat(h1.w);
    w.qs.n = b.Load(a + 172);
    const uint2 em = b.Load2(a + 416);
    w.emitK = em.x;
    w.emitCum = asfloat(em.y);
    const uint4 tr = b.Load4(a + 424), qa = b.Load4(a + 440);
    w.traceA = asfloat(tr.x);
    w.traceB = asfloat(tr.y);
    w.traceResume = tr.z;
    w.queryI = tr.w;
    w.queryA = asfloat(qa.x);
    w.queryB = asfloat(qa.y);
    w.querySum = float3(asfloat(qa.z), asfloat(qa.w), asfloat(b.Load(a + 456)));
    [unroll] for (uint i = 0; i < kRtMaxQueries; ++i)
    {
        const uint q = a + 176 + i * 60;
        const uint4 q0 = b.Load4(q + 0), q1 = b.Load4(q + 16), q2 = b.Load4(q + 32);
        const uint3 q3 = b.Load3(q + 48);
        RtQuery y;
        y.p = asfloat(q0.xyz);
        y.o = float3(asfloat(q0.w), asfloat(q1.x), asfloat(q1.y));
        y.dir = float3(asfloat(q1.z), asfloat(q1.w), asfloat(q2.x));
        y.tfar = asfloat(q2.y);
        y.dist = asfloat(q2.z);
        y.weight = float3(asfloat(q2.w), asfloat(q3.x), asfloat(q3.y));
        y.kind = q3.z;
        w.qs.q[i] = y;
    }
    return w;
}

// Appends a slot to a stage's list.
void waveAppend(RWByteAddressBuffer wave, RWByteAddressBuffer lists, uint maxSlots, uint stage, uint slot)
{
    uint at;
    wave.InterlockedAdd(stage * 4, 1u, at);
    lists.Store((stage * maxSlots + at) * 4, slot);
}

// The slot of this thread in a stage's running dispatch (false past its count).
bool waveSlotOf(RWByteAddressBuffer wave, RWByteAddressBuffer lists, uint maxSlots, uint stage, uint thread, out uint slot)
{
    slot = 0;
    if (thread >= wave.Load(16 + stage * 4)) return false;
    slot = lists.Load((stage * maxSlots + thread) * 4);
    return true;
}

// The pixel of a slot and its sampler seed (the non-staged kernel's).
void wavePixel(RtConstants C, WaveRect r, uint slot, out uint x, out uint y, out uint half_, out uint pi, out uint pixelSeed)
{
    const uint pixels = r.width * r.height, inRect = slot % pixels;
    half_ = r.firstHalf + slot / pixels;
    x = r.x0 + inRect % r.width;
    y = r.y0 + inRect / r.width;
    pi = y * C.width + x;
    pixelSeed = rtHashCombine(rtHashCombine(C.seedLo ^ C.seedHi, half_), pi);
}

// Sample si of the pixel: the camera ray (the order of sample dimensions of the non-staged kernel) and a new path.
RtPath waveStartSample(RtConstants C, uint x, uint y, uint pixelSeed, uint si, out RtSampler smp)
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

// An iteration's end after its sections (and queries): the bounce count advances and the slot goes to Trace, where the
// next iteration starts (or the path's end is taken).
void waveEndIteration(RWByteAddressBuffer wave, RWByteAddressBuffer lists, RWByteAddressBuffer states, uint maxSlots, uint slot, inout WaveSlot w)
{
    if (w.qs.n > 0)
    {
        w.queryI = kRtNone;
        waveStore(states, slot, w);
        waveAppend(wave, lists, maxSlots, kWaveQuery, slot);
        return;
    }
    w.p.bounce += 1;
    waveStore(states, slot, w);
    waveAppend(wave, lists, maxSlots, kWaveTrace, slot);
}

// Calibration (mode 3, GpuPathTracer::calibrate, on each stage kernel's own pipeline, so with its occupancy): fixed work
// per thread - an atmosphere quadrature of sampleEnd panels and pathBase alpha tests of (instance w, geometry h, primitive
// passIndex) - its sums to y0 (16 bytes per thread).
void waveCalibrate(RtConstants C, uint thread)
{
    RWByteAddressBuffer out_ = ResourceDescriptorHeap[g_root.y0];
    const float3 o = float3(0, 1 + (float)thread * 1e-3f, 0), d = normalize(float3(1, 0.05f, 0));
    const float3 tau = rtAtmIntegrate(C.atm, o, d, 0, 50000.0f, g_root.sampleEnd);
    uint opaque = 0;
    [loop] for (uint k = 0; k < g_root.pathBase; ++k)
        opaque += rtAlphaOpaque(g_root.w, g_root.h, g_root.passIndex, frac((float)(k + thread) * 0.6180339f), frac((float)k * 0.3819660f) * 0.5f) ? 1u : 0u;
    out_.Store4(thread * 16, uint4(asuint(tau), opaque));
}
#endif
