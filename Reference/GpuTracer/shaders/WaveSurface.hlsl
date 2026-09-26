// unx-kernel: cs_6_6 main
// Wavefront stage Surface (Wave.hlsli): section 3 (rtPathSurface: the segment's end - space with the sun, or a surface or
// ground vertex with its next-event queries, the next direction and Russian roulette) -> the iteration's end. Worst per
// slot: a surface's textures, one light choice over a light-grid cell and a BSDF sample.
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
    if (waveSlotOf(wave, lists, maxSlots, kWaveSurface, id.x, slot))
    {
        WaveSlot w = waveLoad(states, slot);
        rtPathSurface(C, w.p, w.smp, w.qs, w.g);
        waveEndIteration(wave, lists, states, maxSlots, slot, w);
    }
    rtFlushCounters(0, 0, 0);
}
