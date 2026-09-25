// unx-kernel: cs_6_6 main
// Host boundary probe: a dependent pass with negligible work (the render graph's fixed per-pass cost, ARCHITECTURE 1.4).
// P[0].x counter UAV (raw), P[0].y pass index
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].x];
    const uint offset = (tid & 63) * 4;
    counter.Store(offset, counter.Load(offset) + P[0].y + 1);
}
