// unx-kernel: cs_6_6 main
// Clears the classification atlas (VsmCls.hlsli) to 0 = no caster before the frame's conservative raster.
// P[0] = { atlas UAV (raw), words, 0, 0 }
#include "Bindless.hlsli"

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const uint w = id * 4;
    if (w >= P[0].y) return;
    RWByteAddressBuffer atlas = ResourceDescriptorHeap[P[0].x];
    atlas.Store4(w * 4, uint4(0, 0, 0, 0));
}
