// unx-kernel: cs_6_6 main
// Clears the deterministic selection state (gi.deterministic) before level 0.
// P[0] = { state UAV (raw), words, 0, 0 }
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    state.Store(i * 4, 0u);
}
