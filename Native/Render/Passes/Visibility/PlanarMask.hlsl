// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Planar reflection view mask (ViewDesc::planarMask, R8_UINT, nonzero = mirror pixel) -> the view's tile cull mask
// (P[1].z px tiles, one bit per tile, row major: CullView cullMaskOffset 0, tilePx; VisibilityCommon.hlsli tile masks).
// Clusters over tiles without mirror pixels are culled like a raster-service request's.
//   MODE=0: zero the mask words (one thread per word).
//   MODE=1: one 8 x 8 group per tile (each thread covers tile / 8 squared pixels): its bit is set when any pixel of the
//           tile is a mirror pixel.
//   P[0] mask SRV (Texture2D<uint>), bits UAV (raw), view width, view height; P[1] tiles per row, words, tile px, 0
#include "Bindless.hlsli"

#if MODE == 0
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[1].y) return;
    RWByteAddressBuffer bits = ResourceDescriptorHeap[P[0].y];
    bits.Store(4 * i, 0);
}
#else
groupshared uint gs_any;

[numthreads(8, 8, 1)]
void main(uint2 gid : SV_GroupID, uint2 lane : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if (index == 0) gs_any = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint tilePx = P[1].z, step = tilePx / 8;
    Texture2D<uint> mask = ResourceDescriptorHeap[P[0].x];
    bool any = false;
    for (uint y = 0; y < step && !any; ++y)
        for (uint x = 0; x < step && !any; ++x)
        {
            const uint2 p = gid * tilePx + lane * step + uint2(x, y);
            any = p.x < P[0].z && p.y < P[0].w && mask[p] != 0;
        }
    if (any) gs_any = 1;
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && gs_any != 0)
    {
        const uint tile = gid.y * P[1].x + gid.x;
        RWByteAddressBuffer bits = ResourceDescriptorHeap[P[0].y];
        bits.InterlockedOr(4 * (tile >> 5), 1u << (tile & 31));
    }
}
#endif
