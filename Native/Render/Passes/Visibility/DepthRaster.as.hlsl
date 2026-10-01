// unx-kernel: as_6_6 main
// unx-variants: TILE=1,2
// Depth raster service, tile-local runs (DepthRasterRequest::tileLocal; TILE=2 the atlas mode): one 32-thread
// amplification group per draw-list entry (a visible cluster and its tile rectangle, CullClusters). It counts the pairs
// of each row of the rectangle in the request's tile mask (DepthRasterPayload.hlsli), keeps the per-row prefix in the
// payload and launches one DepthRaster.ms group per pair, which finds its pair from the prefix. The raster work per pair
// is the one of the stored pair list before (same pairs, same clipping); the pairs have no list and no capacity.
// The prefix is built in groupshared memory (no wave intrinsics: the group may span several waves on any GPU).
//   P[0] visible SRV (uint2), lists SRV (raw), state SRV (raw), list
//   P[1] phase (1), list capacity, views SRV, viewport per view
//   P[2] tile rectangles SRV (uint2 per visible entry: packTileRect, bit 31 of .y = whole), atlas slots SRV, atlas tiles
//        per row, atlas size
//   P[3] tile mask SRV (raw: DepthRasterRequest::cullMask)
#include "Passes/Visibility/DepthRasterPayload.hlsli"

groupshared DepthRasterPayload g_payload;
groupshared uint g_rowPairs[DR_MAX_ROWS];

[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint index = group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint visibleIndex = valid ? lists.Load(4 * (list * capacity + index)) & ~LIST_ENTRY_MIXED : 0;
    StructuredBuffer<uint2> rects = ResourceDescriptorHeap[P[2].x];
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 packed = valid ? rects[visibleIndex] : uint2(0, 0);
    const uint view = valid ? visible[visibleIndex].y >> 24 : 0;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[view];
    const uint2 a = uint2(packed.x & 0xFFFFu, packed.x >> 16), b = uint2(packed.y & 0xFFFFu, (packed.y >> 16) & 0x7FFFu);
    const bool whole = (packed.y & 0x80000000u) != 0;
    const uint rows = valid && !whole ? min(b.y - a.y + 1, DR_MAX_ROWS) : 0;
    const bool single = (v.flags & CULL_VIEW_TILE_SINGLE) != 0;
    ByteAddressBuffer mask = ResourceDescriptorHeap[P[3].x];
    for (uint r = lane; r < rows; r += 32) g_rowPairs[r] = drRowCount(mask, v.cullMaskOffset, v.tilesX, a.y + r, a.x, b.x, single);
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        uint running = 0;
        for (uint r = 0; r < rows; ++r)
        {
            g_payload.prefix[r] = running;
            running += g_rowPairs[r];
        }
        g_payload.visibleIndex = visibleIndex;
        g_payload.whole = whole ? 1u : 0u;
        g_payload.total = !valid ? 0u : (whole ? 1u : running);
        g_payload.rectLo = packed.x;
        g_payload.rectHi = packed.y & 0x7FFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint total = g_payload.total;
    DispatchMesh(min(total, DR_GRID), (total + DR_GRID - 1) / DR_GRID, 1, g_payload);
}
