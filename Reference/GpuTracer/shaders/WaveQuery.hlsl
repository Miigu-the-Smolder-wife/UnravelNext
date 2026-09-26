// unx-kernel: cs_6_6 main
// Wavefront stage Query (Wave.hlsli): the iteration's next-event queries (rtResolveQueries' terms in order: visibility and
// transmittance, their sum added to the path's radiance as the CPU adds it) -> the iteration's end. The shadow rays are
// resumable (rtResolveQueryPart: at most kWaveCandidatesPerVisit alpha candidates per ray per visit; a slot whose ray is not
// done comes back to Query). Worst per slot: <= 4 ray steps and <= 4 atmosphere quadratures of <= 50 panels (or tables).
#include "Wave.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const RtConstants C = rtC();
    if (g_root.sampleBegin == 3)
    {
        waveCalibrate(C, id.x);
        return;
    }
    RWByteAddressBuffer wave = ResourceDescriptorHeap[g_root.x0];
    RWByteAddressBuffer states = ResourceDescriptorHeap[g_root.y0];
    RWByteAddressBuffer lists = ResourceDescriptorHeap[g_root.w];
    const uint maxSlots = g_root.h;
    uint slot;
    if (waveSlotOf(wave, lists, maxSlots, kWaveQuery, id.x, slot))
    {
        WaveSlot w = waveLoad(states, slot);
        if (w.queryI == kRtNone)
        {
            w.queryI = 0;
            w.querySum = float3(0, 0, 0);
            w.queryA = 0;
            w.queryB = w.qs.q[0].tfar;
        }
        bool done = true;
        [loop] for (uint k = 0; k < kRtMaxQueries && w.queryI < w.qs.n; ++k)
        {
            float3 contrib;
            if (!rtResolveQueryPart(C, w.qs.q[w.queryI], w.queryA, w.queryB, kWaveCandidatesPerVisit, contrib))
            {
                done = false;
                break;
            }
            w.querySum += contrib;
            if (++w.queryI < w.qs.n)
            {
                w.queryA = 0;
                w.queryB = w.qs.q[w.queryI].tfar;
            }
        }
        if (!done)
        {
            waveStore(states, slot, w);
            waveAppend(wave, lists, maxSlots, kWaveQuery, slot);  // the shadow ray's next interval in the next round
        }
        else
        {
            w.p.L += w.querySum;
            w.qs.n = 0;
            waveEndIteration(wave, lists, states, maxSlots, slot, w);
        }
    }
    rtFlushCounters(0, 0, 0);
}
