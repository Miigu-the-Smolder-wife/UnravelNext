// unx-kernel: cs_6_6 main
// Wavefront stage Query (Wave.hlsli): the iteration's next-event queries (rtResolveQueries: visibility and transmittance,
// their sum added to the path's radiance as the CPU adds it) -> the iteration's end. Worst per slot: <= 4 shadow rays
// (alpha candidates <= kRtMaxCandidates each) and <= 4 atmosphere quadratures of <= 50 panels (or table lookups).
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
        if (w.qs.n > 0) w.p.L += rtResolveQueries(C, w.qs);
        waveEndIteration(wave, lists, states, maxSlots, slot, w);
    }
    rtFlushCounters(0, 0, 0);
}
