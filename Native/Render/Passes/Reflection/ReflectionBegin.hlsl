// unx-kernel: cs_6_6 main
// Reflection frame setup: tile validity rows of view.reflection to 0 (untouched tiles are K), job counter to 0.
// P[0] = { reflection UAV, arguments UAV (raw), tiles x, tiles y }, P[1] = { pixel rows H, 0, 0, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_DispatchThreadID)
{
    if (all(tile == 0))
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
        args.Store(0, 0u);
    }
    if (tile.x >= P[0].z || tile.y >= P[0].w) return;
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    reflection[uint2(tile.x, P[1].x + tile.y)] = float4(0, 0, 0, 0);
}
