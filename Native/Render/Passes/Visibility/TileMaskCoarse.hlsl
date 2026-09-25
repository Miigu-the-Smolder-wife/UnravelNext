// unx-kernel: cs_6_6 main
// Coarse tile mask of a raster-service run: bit c of a view = OR of the 8 x 8 tiles of coarse cell c (row major,
// ceil(tilesX / 8) cells per row), so the cull kernels enumerate set tiles in time proportional to the set cells a
// cluster's rectangle touches rather than to its area (CullShared.hlsli, tileVisit*). One 64-thread group per 64
// cells of one view (group.y): two words, no clearing needed.
//   P[0] views SRV, tile mask SRV (raw), coarse mask UAV (raw), coarse words per view
#include "Passes/Visibility/VisibilityCommon.hlsli"

groupshared uint g_words[2];

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[0].x];
    const CullView v = views[group.y];
    if (lane < 2) g_words[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint tilesY = ((uint)v.viewportSize.y + v.tilePx - 1) / v.tilePx;
    const uint coarseX = (v.tilesX + 7) / 8, coarseY = (tilesY + 7) / 8;
    const uint cell = group.x * 64 + lane;
    bool set = false;
    if (v.cullMaskOffset != UNX_NONE && cell < coarseX * coarseY)
    {
        ByteAddressBuffer mask = ResourceDescriptorHeap[P[0].y];
        const uint2 lo = uint2(cell % coarseX, cell / coarseX) * 8;
        const uint2 hi = min(lo + 7, uint2(v.tilesX, tilesY) - 1);
        for (uint y = lo.y; y <= hi.y && !set; ++y)
        {
            const uint l = y * v.tilesX + lo.x, h = y * v.tilesX + hi.x;
            for (uint w = l >> 5; w <= (h >> 5); ++w) set = set || tileMaskBits(mask, v.cullMaskOffset, w, l, h) != 0;
        }
    }
    if (set) InterlockedOr(g_words[lane >> 5], 1u << (lane & 31));
    GroupMemoryBarrierWithGroupSync();
    if (lane < 2)
    {
        RWByteAddressBuffer coarse = ResourceDescriptorHeap[P[0].z];
        coarse.Store(4 * (group.y * P[0].w + group.x * 2 + lane), g_words[lane]);
    }
}
