// unx-kernel: cs_6_6 main
// Requests updates for the entries last frame's GI rays hit (their irradiance fed bounces), after the screen probes'.
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint parity = (h.frame & 1u) ^ 1u;
    const uint count = min(parity == 0 ? h.hitCount0 : h.hitCount1, h.capacity);
    if (i >= count) return;
    const uint entry = b.Load(h.offHitList + (parity * h.capacity + i) * 4);
    if (b.Load(h.offMeta + entry * 16 + 4) == 0) return;  // freed since
    giRequestUpdate(b, h, entry);
}
