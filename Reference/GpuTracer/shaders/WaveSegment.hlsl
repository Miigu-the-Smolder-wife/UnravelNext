// unx-kernel: cs_6_6 main
// Wavefront stage Segment (Wave.hlsli): section 2 (rtPathMedium: the area-light emission along the segment and the
// atmosphere on it) -> Surface, or the iteration's end (a medium scattering event, or the path ended). Worst per slot: the
// area lights of one light-grid cell and <= 4 atmosphere quadratures of <= 50 panels (one more of <= 64 in a valley).
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
    if (waveSlotOf(wave, lists, maxSlots, kWaveSegment, id.x, slot))
    {
        WaveSlot w = waveLoad(states, slot);
        if (rtPathMedium(C, w.p, w.smp, w.qs, w.g))
        {
            waveStore(states, slot, w);
            waveAppend(wave, lists, maxSlots, kWaveSurface, slot);
        }
        else waveEndIteration(wave, lists, states, maxSlots, slot, w);
    }
    rtFlushCounters(0, 0, 0);
}
