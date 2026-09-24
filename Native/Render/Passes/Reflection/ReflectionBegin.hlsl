// unx-kernel: cs_6_6 main
// Reflection frame setup: tile validity rows of view.reflection to 0 (untouched tiles are K), job counter to 0.
// Also zeroes the planar candidates' pixel counts (64).
// P[0] = { reflection UAV, arguments UAV (raw), tiles x, tiles y }, P[1] = { pixel rows H, planar counts UAV, 0, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]  // dispatched over at least 64 x 1 threads (the counts)
void main(uint2 tile : SV_DispatchThreadID)
{
    if (all(tile == 0))
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
        args.Store4(0, uint4(0, 0, 0, 0));  // job counter, statistics
    }
    if (tile.y == 0 && tile.x < 64)
    {
        RWByteAddressBuffer counts = ResourceDescriptorHeap[P[1].y];
        counts.Store(tile.x * 4, 0u);
    }
    if (tile.x >= P[0].z || tile.y >= P[0].w) return;
    RWTexture2D<float4> reflection = ResourceDescriptorHeap[P[0].x];
    reflection[uint2(tile.x, P[1].x + tile.y)] = float4(0, 0, 0, 0);
}
