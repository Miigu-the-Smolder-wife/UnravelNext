// unx-kernel: cs_6_6 main
// Wavefront stage Trace (Wave.hlsli): an iteration's start. A path that has ended (or reached kRtMaxBounces) adds its
// sample to the slot's float sum and the slot starts its next sample, or after its last adds the sum to the half's double
// accumulator and leaves the lists; then section 1 (rtPathTrace: the segment) -> Segment, or the iteration's end.
// The segment's ray is resumable (rtIntersectPart: at most kWaveCandidatesPerVisit alpha candidates per visit; a ray with
// more comes back to Trace until done, the iteration's start not repeated). Mode 0 (start): thread = slot, every slot of the
// rectangle starts its first sample. Worst per slot: one ray step of <= kWaveCandidatesPerVisit candidates and a camera ray.
#include "Wave.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    if (g_root.sampleBegin == 3)  // calibration (uniform over the dispatch)
    {
        waveCalibrate(C, id.x);
        return;
    }
    RWByteAddressBuffer wave = ResourceDescriptorHeap[g_root.x0];
    RWByteAddressBuffer states = ResourceDescriptorHeap[g_root.y0];
    RWByteAddressBuffer lists = ResourceDescriptorHeap[g_root.w];
    const uint maxSlots = g_root.h, mode = g_root.sampleBegin;
    const WaveRect r = waveRect(wave);
    uint nans = 0, truncated = 0;
    uint slot = id.x;
    const bool have = mode == 0 ? id.x < r.width * r.height * r.halves : waveSlotOf(wave, lists, maxSlots, kWaveTrace, id.x, slot);
    if (have)
    {
        uint x, y, half_, pi, pixelSeed;
        wavePixel(C, r, slot, x, y, half_, pi, pixelSeed);
        WaveSlot w;
        bool active = true;
        if (mode == 0)
        {
            w.traceResume = 0;
            w.si = r.c0;
            w.acc = float3(0, 0, 0);
            w.p = waveStartSample(C, x, y, pixelSeed, w.si, w.smp);
        }
        else
        {
            w = waveLoad(states, slot);
            if (w.traceResume == 0)  // not a resumed ray: the iteration's start
            {
            // rtPathStep's top: an ended path (or one at the iteration cap) is its sample's value
            if (w.p.alive && w.p.bounce >= kRtMaxBounces)
            {
                w.p.truncated = true;
                w.p.alive = false;
            }
            if (!w.p.alive)
            {
                truncated += w.p.truncated ? 1 : 0;
                if (rtFinite3(w.p.L)) w.acc += w.p.L;
                else ++nans;
                if (++w.si >= r.c1)
                {
                    RWByteAddressBuffer accum = ResourceDescriptorHeap[half_ == 0 ? C.b.accum0 : C.b.accum1];
                    rtAccumulateDouble3(accum, pi, w.acc);
                    active = false;
                }
                else w.p = waveStartSample(C, x, y, pixelSeed, w.si, w.smp);
            }
            }
        }
        if (active)
        {
            if (w.traceResume == 0)
            {
                w.qs.n = 0;
                w.traceA = w.p.tmin;
                w.traceB = kRtFarT;
            }
            RtHit hit;
            bool found;
            if (!rtIntersectPart(w.p.o, w.p.d, w.traceA, w.traceB, kRtFarT, kRtMaskAll, kWaveCandidatesPerVisit, hit, found))
            {
                w.traceResume = 1;  // the ray's next interval in the next round
                waveStore(states, slot, w);
                waveAppend(wave, lists, maxSlots, kWaveTrace, slot);
            }
            else
            {
                w.traceResume = 0;
                if (rtPathTraceGiven(C, w.p, w.g, found, hit))
                {
                    w.emitK = kRtNone;
                    w.emitCum = 0;
                    waveStore(states, slot, w);
                    waveAppend(wave, lists, maxSlots, kWaveSegment, slot);
                }
                else waveEndIteration(wave, lists, states, maxSlots, slot, w);
            }
        }
    }
    rtFlushCounters(nans, truncated, 0);
}
