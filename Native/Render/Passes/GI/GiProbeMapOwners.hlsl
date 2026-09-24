// unx-kernel: cs_6_6 main
// Radiance map owners (ScreenProbes.hlsli): thread per probe. Neighbouring probes mostly share a cache entry; the entry's
// owner (lowest probe index reading it, GiProbeGather) gets the map copy, every other probe only its header texel (4, 3) =
// { map frame normal, owner probe x | y << 16 }. Owners (and probes without an entry, whose map is zero) are appended
// (one atomic per wave) to the list GiProbeMaps runs over indirectly: args = { 512, ceil(owners / 512), 1 }.
// P[0] = { cache SRV (raw), probes UAV, probesX, probesY }, P[1] = { owner list UAV (raw: count, then probe indices),
// dispatch args UAV (raw), 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    const bool inside = probe.x < P[0].z && probe.y < P[0].w;
    bool owns = false;
    if (inside)
    {
        ByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
        RWTexture2D<uint4> t = ResourceDescriptorHeap[P[0].y];
        const GiHeader h = giHeader(b);
        const uint entry = t[uint2(probe.x * 8 + 5, probe.y * 4 + 3)].x;
        const uint self = probe.y * P[0].z + probe.x;
        const uint owner = entry != GI_ENTRY_PENDING ? b.Load(b.Load(GI_H_MAP_OWNER) + entry * 4) : self;
        owns = owner == self;
        if (!owns) t[uint2(probe.x * 8 + 4, probe.y * 4 + 3)] = uint4(giPackNormal(giAnchorNormal(b, h, entry)), (owner % P[0].z) | ((owner / P[0].z) << 16), 0, 0);
    }
    const uint n = WaveActiveCountBits(owns);
    if (n == 0) return;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[1].x];
    uint base = 0;
    if (WaveIsFirstLane())
    {
        list.InterlockedAdd(0, n, base);
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].y];
        args.InterlockedMax(4, (base + n + 511) / 512);
    }
    base = WaveReadLaneFirst(base);
    if (owns) list.Store(4 + (base + WavePrefixCountBits(owns)) * 4, probe.y * P[0].z + probe.x);
}
