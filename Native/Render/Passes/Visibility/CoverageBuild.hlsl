// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,5,4
// Coverage layer bookkeeping (CoverageLayer.hlsli root constants; layout CoverageTiles.hlsli). The raster appends into
// the tiles directly; there is no sort (M's composite sorts a tile's records in groupshared):
//   MODE=0 (1 thread, after culling): DispatchMesh args over the band B list (both phases) and the clear args over last
//          frame's tiles (the tile list still holds them: one group per tile).
//   MODE=1 (one group per last frame's tile): the tile's header and chunk table back to empty, so the whole-screen
//          buffers are never cleared.
//   MODE=2 (1 thread, after the raster): the tile list's header for M: DispatchIndirect args (one group per tile),
//          tile, fragment and chunk counts, the layout words; V's args over the listed tiles for MODE 3.
//   MODE=3 (one group per listed tile): the opaqueCovered bits from the tile's records (per pixel in groupshared: the
//          union of the opaque records' masks and the farthest of their depths; set where the union is full and the band
//          A surface lies behind that depth; COVERAGE_REDESIGN 4.6); a tile with more fragments than the heavy threshold
//          goes on the heavy list.
//   MODE=5 (1 thread): the heavy list's DispatchIndirect args (one group per heavy tile) and count.
//   MODE=4 (one group per tile of the view; on creation): every tile empty; the tile list's header zeroed.
#include "Passes/Visibility/CoverageLayer.hlsli"

void storeDispatchTiles(RWByteAddressBuffer args, uint word, uint tiles)
{
    args.Store3(4 * word, uint3(min(tiles, 65535u), (tiles + 65534u) / 65535u, 1));
}

void storeDispatch64(RWByteAddressBuffer args, uint word, uint items) { storeDispatchTiles(args, word, (items + 63) / 64); }

// One tile back to empty, by its 64 threads: header, chunk table.
void clearTile(uint tile, uint lane)
{
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer table = ResourceDescriptorHeap[COV_CHUNK_TABLE];
    if (lane < COV_TILE_WORDS) headers.Store(4 * (tile * COV_TILE_WORDS + lane), lane == COV_TILE_ZFAR ? 0xFFFFFFFFu : 0u);
    for (uint c = lane; c < COV_TABLE_SLOTS; c += 64) table.Store(4 * (tile * COV_TABLE_SLOTS + c), 0);
}

#if MODE == 0 || MODE == 2 || MODE == 5
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_ARGS];
    RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
#if MODE == 0
    const uint entries = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), COV_LIST_CAPACITY);
    args.Store3(4 * VA_COV_MESH, uint3(min(entries, 65535u), (entries + 65534u) / 65535u, 1));
    storeDispatchTiles(args, VA_COV_CLEAR, min(list.Load(4 * COV_LIST_COUNT), COV_TILES));
#elif MODE == 2
    const uint tiles = min(state.Load(4 * VS_COV_TILES), COV_TILES);
    storeDispatchTiles(list, COV_LIST_ARGS, tiles);
    list.Store(4 * COV_LIST_COUNT, tiles);
    list.Store(4 * COV_LIST_FRAGMENTS, state.Load(4 * VS_COV_FRAGMENTS));
    list.Store(4 * COV_LIST_CHUNKS, min(state.Load(4 * VS_COV_CHUNKS), COV_CAP_CHUNKS));
    list.Store(4 * COV_LIST_TABLE_SLOTS, COV_TABLE_SLOTS);
    list.Store(4 * COV_LIST_TILES_X, COV_TILES_X);
    list.Store(4 * COV_LIST_HEAVY_MIN, COV_HEAVY_MIN);
    list.Store(4 * COV_LIST_HEAVY_START, COV_LIST_TILES + COV_TILES);
    storeDispatchTiles(args, VA_COV_TILES, tiles);
    state.Store(4 * VS_COV_POOL, COV_CAP_CHUNKS);
#else
    const uint heavy = min(state.Load(4 * VS_COV_HEAVY), COV_TILES);
    storeDispatchTiles(list, COV_LIST_HEAVY_ARGS, heavy);
    list.Store(4 * COV_LIST_HEAVY_COUNT, heavy);
#endif
}
#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint i = gid.x + gid.y * 65535u;
    if (i >= min(list.Load(4 * COV_LIST_COUNT), COV_TILES)) return;
    clearTile(list.Load(4 * (COV_LIST_TILES + i)), lane);
}
#elif MODE == 3
groupshared uint gs_union[COV_TILE_PIXELS];
groupshared uint gs_farthest[COV_TILE_PIXELS];
groupshared uint gs_covered[2];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer table = ResourceDescriptorHeap[COV_CHUNK_TABLE];
    RWStructuredBuffer<uint4> records = ResourceDescriptorHeap[COV_RECORDS];
    const uint i = gid.x + gid.y * 65535u;
    if (i >= min(state.Load(4 * VS_COV_TILES), COV_TILES)) return;  // uniform over the group
    const uint tile = list.Load(4 * (COV_LIST_TILES + i));
    gs_union[lane] = 0;
    gs_farthest[lane] = 0xFFFFFFFFu;
    if (lane < 2) gs_covered[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    // The tile's records chunk by chunk (one record per thread, 1 KB contiguous per chunk): the table's chunks, then the
    // extension chain in order.
    const uint h = 4 * tile * COV_TILE_WORDS, count = headers.Load(h + 4 * COV_TILE_COUNT), slots = COV_TABLE_SLOTS;
    uint ext = headers.Load(h + 4 * COV_TILE_EXT), e = 0;
    for (uint c = 0; c * COV_CHUNK_RECORDS < count; ++c)
    {
        uint chunk = 0;
        if (c < slots) chunk = table.Load(4 * (tile * slots + c));
        else
        {
            if (e == COV_EXT_SLOTS)
            {
                ext = ext != 0 ? records[(ext - 1) * COV_CHUNK_RECORDS + COV_EXT_SLOTS / 4][COV_EXT_SLOTS % 4] : 0;
                e = 0;
            }
            chunk = ext != 0 ? records[(ext - 1) * COV_CHUNK_RECORDS + e / 4][e % 4] : 0;
            ++e;
        }
        if (chunk != 0 && c * COV_CHUNK_RECORDS + lane < count)
        {
            const uint4 r = records[(chunk - 1) * COV_CHUNK_RECORDS + lane];
            const uint p = r.w >> 26;
            if ((r.y & COV_DEPTH_SEE_THROUGH) == 0 && r.z != 0)
            {
                InterlockedOr(gs_union[p], r.z);
                InterlockedMin(gs_farthest[p], r.y);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = uint2(tile % COV_TILES_X, tile / COV_TILES_X) * COV_TILE_PX + uint2(lane % COV_TILE_PX, lane / COV_TILE_PX);
    if (pixel.x < COV_WIDTH && pixel.y < COV_HEIGHT && gs_union[lane] == COV_MASK_FULL)
    {
        Texture2D<float> depthA = ResourceDescriptorHeap[COV_BAND_A_DEPTH];
        if (asuint(depthA.Load(int3(pixel, 0))) < gs_farthest[lane]) InterlockedOr(gs_covered[lane / 32], 1u << (lane % 32));
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        headers.Store(h + 4 * COV_TILE_OPAQUE_LO, gs_covered[0]);
        headers.Store(h + 4 * COV_TILE_OPAQUE_HI, gs_covered[1]);
        if (count > COV_HEAVY_MIN)
        {
            uint slot = 0;
            state.InterlockedAdd(4 * VS_COV_HEAVY, 1, slot);
            if (slot < COV_TILES) list.Store(4 * (COV_LIST_TILES + COV_TILES + slot), tile);
        }
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    const uint tile = gid.x + gid.y * COV_TILES_X;
    if (tile == 0 && lane < COV_LIST_TILES)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
        list.Store(4 * lane, 0);
    }
    clearTile(tile, lane);
}
#endif
