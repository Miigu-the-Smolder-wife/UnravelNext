// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5,6,7,8,9,10
// Coverage layer bookkeeping (CoverageLayer.hlsli: root constants and the pass order; layout CoverageTiles.hlsli). The
// raster appends to a stream; these passes sort it into the tiles' pixel-major ranges and derive the per-pixel products.
// Every pass does a fixed amount of work per group and takes as many groups as the data needs (INTERFACES 3.6):
//   MODE=0 (1 thread, after culling): DispatchMesh args over the band B list (both phases) and the clear args over last
//          frame's tiles (the tile list still holds them).
//   MODE=1 (one group per last frame's tile): its header, pixel counters and depth range back to empty, so the
//          whole-screen buffers are never cleared.
//   MODE=2 (1 thread, after the raster): count and scatter args over the stored stream entries.
//   MODE=6 count (one group per COV_BLOCK stream entries): per key (tile x 64 + pixel) and per tile, one atomic for the
//          lanes of a wave sharing it (WaveMatch); a tile's first record puts it on the tile list.
//   MODE=7 scan (one group of 1,024 threads, each over at most ceil(tiles / 1024) listed tiles): record and block bases,
//          an exclusive prefix in list order; tiles of more than one block get a scratch slot; the list header.
//   MODE=9 offsets (one group per listed tile): the pixels' starts inside the tile (64-wide prefix); the counters set to
//          the pixels' absolute ends; the tile's scratch slot initialised.
//   MODE=8 scatter (one group per COV_BLOCK stream entries): each record to its pixel's range (one decrement per key and
//          wave; a pixel's records land in no defined order); hair and stream records also get an entry in the special
//          list (v1.73, A's request: their owners shade them before the composite): { final record element, kind }, one
//          atomic per wave, in no defined order.
//   MODE=10 (1 thread, after the scatter): the special list's header (count clamped, DispatchIndirect args).
//   MODE=3 blocks (one group per block of COV_BLOCK records): per pixel the opaque mask union, the farthest opaque depth,
//          the nearest and farthest depth; a one-block tile finishes here, a longer one merges into its scratch slot.
//   MODE=5 heavy (one group per tile of more than one block): finishes it from its scratch slot.
//   MODE=4 (one group per tile of the view; on creation): every tile, pixel counter and depth range empty; the tile
//          list's header zeroed.
// "Finishing" a tile writes coverageDepthRange for its pixels and its opaqueCovered bits (the union of the pixel's opaque
// record masks is full and the band A surface lies behind the farthest of those records; COVERAGE_REDESIGN 4.6).
#include "Passes/Visibility/CoverageBuckets.hlsli"  // (the tile clears empty the depth buckets' cover too: MODE 1, 4 take it in P[0].z)

void storeDispatch(RWByteAddressBuffer args, uint word, uint groups)
{
    args.Store3(4 * word, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1));
}

uint2 tilePixel(uint tile, uint lane)
{
    return uint2(tile % COV_TILES_X, tile / COV_TILES_X) * COV_TILE_PX + uint2(lane % COV_TILE_PX, lane / COV_TILE_PX);
}

// One tile back to empty, by its 64 threads: header, pixel counters, depth range.
void clearTile(uint tile, uint lane)
{
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_COUNTERS];
    if (lane < COV_TILE_WORDS) headers.Store(4 * (tile * COV_TILE_WORDS + lane), 0);
    counters.Store(4 * (tile * COV_TILE_PIXELS + lane), 0);
    if (COV_COVER != UNX_NONE)  // the depth buckets' cover of the pixel: no union, no fragment
    {
        RWByteAddressBuffer cover = ResourceDescriptorHeap[COV_COVER];
        cover.Store2(8 * (tile * COV_TILE_PIXELS + lane), uint2(0, 0));
    }
    const uint2 pixel = tilePixel(tile, lane);
    if (pixel.x < COV_WIDTH && pixel.y < COV_HEIGHT)
    {
        RWTexture2D<uint2> range = ResourceDescriptorHeap[COV_DEPTH_RANGE];
        range[pixel] = uint2(0, 0xFFFFFFFFu);
    }
}

// Lanes of the wave below this one, and helpers over WaveMatch masks.
uint4 lanesBelow()
{
    const uint lane = WaveGetLaneIndex();
    uint4 m;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint lo = 32 * k;
        m[k] = lane >= lo + 32 ? 0xFFFFFFFFu : (lane > lo ? (1u << (lane - lo)) - 1u : 0u);
    }
    return m;
}
uint count4(uint4 m) { return countbits(m.x) + countbits(m.y) + countbits(m.z) + countbits(m.w); }
uint firstLane4(uint4 m) { return m.x != 0 ? firstbitlow(m.x) : (m.y != 0 ? 32 + firstbitlow(m.y) : (m.z != 0 ? 64 + firstbitlow(m.z) : 96 + firstbitlow(m.w))); }

// One pixel of a tile finished: its depth range; returns its opaqueCovered bit.
bool finishPixel(uint tile, uint lane, uint unionMask, uint farthestOpaque, uint nearest, uint farthest)
{
    const uint2 pixel = tilePixel(tile, lane);
    if (pixel.x >= COV_WIDTH || pixel.y >= COV_HEIGHT) return false;
    RWTexture2D<uint2> range = ResourceDescriptorHeap[COV_DEPTH_RANGE];
    range[pixel] = uint2(nearest, farthest);
    if (unionMask != COV_MASK_FULL) return false;
    Texture2D<float> depthA = ResourceDescriptorHeap[COV_BAND_A_DEPTH];
    return asuint(depthA.Load(int3(pixel, 0))) < farthestOpaque;  // reversed Z: band A lies behind
}

#if MODE == 0 || MODE == 2
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_ARGS];
#if MODE == 0
    ByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint entries = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), COV_LIST_CAPACITY);
    args.Store3(4 * VA_COV_MESH, uint3(min(entries, 65535u), (entries + 65534u) / 65535u, 1));
    [unroll] for (uint t = LIST_T_BACK; t <= LIST_T_NONE; ++t)  // A6: the translucent lists' records (class 2 pixels)
    {
        const uint te = min(state.Load(4 * (VS_LIST_COUNT + t)), COV_LIST_CAPACITY);
        args.Store3(4 * (VA_COV_T_MESH + 3 * (t - LIST_T_BACK)), uint3(min(te, 65535u), (te + 65534u) / 65535u, 1));
    }
    storeDispatch(args, VA_COV_CLEAR, min(list.Load(4 * COV_LIST_COUNT), COV_TILES));
#else
    const uint stored = min(state.Load(4 * VS_COV_FRAGMENTS), COV_CAP);
    storeDispatch(args, VA_COV_RECORDS, (stored + COV_BLOCK - 1) / COV_BLOCK);
    state.Store(4 * VS_COV_POOL, COV_CAP);
    state.Store(4 * VS_COV_SPECIAL, 0);
#endif
}

#elif MODE == 10
[numthreads(1, 1, 1)]
void main()
{
    if (COV_SPECIAL == UNX_NONE) return;
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer special = ResourceDescriptorHeap[COV_SPECIAL];
    const uint count = min(state.Load(4 * VS_COV_SPECIAL), COV_SPECIAL_CAP);
    special.Store4(0, uint4(count, (count + 63) / 64, 1, 1));
}
#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint j = gid.x + gid.y * 65535u;
    if (j >= min(list.Load(4 * COV_LIST_COUNT), COV_TILES)) return;
    clearTile(list.Load(4 * (COV_LIST_INFO + 4 * j)), lane);
}
#elif MODE == 6 || MODE == 8
groupshared uint gs_end[256];

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[COV_KEYS];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_COUNTERS];
    const uint stored = min(state.Load(4 * VS_COV_FRAGMENTS), COV_CAP);
    const uint first = (gid.x + gid.y * 65535u) * COV_BLOCK;
    if (first >= stored) return;  // uniform over the group
    const uint4 below = lanesBelow();
    const uint keyEnd = COV_TILES * COV_TILE_PIXELS;
#if MODE == 6
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint e = first + q * 256 + gi;
        uint key = e < stored ? keys.Load(4 * e) : 0xFFFFFFFFu;
        if (e < stored && key >= keyEnd)
        {
            state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: the raster writes keys inside the view
            key = 0xFFFFFFFFu;
        }
        const bool valid = key != 0xFFFFFFFFu;
        const uint4 samePixel = WaveMatch(key);
        if (valid && count4(samePixel & below) == 0) counters.InterlockedAdd(4 * key, count4(samePixel));
        const uint tile = key / COV_TILE_PIXELS;
        const uint4 sameTile = WaveMatch(tile);
        if (valid && count4(sameTile & below) == 0)
        {
            uint before = 0;
            headers.InterlockedAdd(4 * (tile * COV_TILE_WORDS + COV_TILE_COUNT), count4(sameTile), before);
            if (before == 0)
            {
                uint slot = 0;
                state.InterlockedAdd(4 * VS_COV_TILES, 1, slot);
                if (slot < COV_TILES)
                {
                    list.Store(4 * (COV_LIST_INFO + 4 * slot), tile);
                    headers.Store(4 * (tile * COV_TILE_WORDS + COV_TILE_LISTED), slot + 1);
                }
                else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: a tile is listed once
            }
        }
    }
#else
    RWStructuredBuffer<uint4> stream = ResourceDescriptorHeap[COV_STREAM];
    RWStructuredBuffer<uint4> records = ResourceDescriptorHeap[COV_RECORDS];
    // The lanes of a wave are consecutive thread indices (gs_end holds one word per thread; a key's leader publishes the
    // end its decrement returned at its own index).
    const uint waveBase = gi - WaveGetLaneIndex();
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint e = first + q * 256 + gi;
        uint key = e < stored ? keys.Load(4 * e) : 0xFFFFFFFFu;
        if (key >= keyEnd) key = 0xFFFFFFFFu;  // (counted as a defect by the count pass)
        const bool valid = key != 0xFFFFFFFFu;
        const uint4 samePixel = WaveMatch(key);
        const uint rank = count4(samePixel & below);
        uint specialElement = 0xFFFFFFFFu;
        if (valid && rank == 0)
        {
            uint end = 0;
            counters.InterlockedAdd(4 * key, 0u - count4(samePixel), end);
            gs_end[gi] = end;
        }
        GroupMemoryBarrierWithGroupSync();
        if (valid)
        {
            const uint dst = gs_end[waveBase + firstLane4(samePixel)] - 1 - rank;
            if (dst < COV_CAP) records[dst] = stream[e];
            else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: counts and stream disagree
            specialElement = dst;
        }
        GroupMemoryBarrierWithGroupSync();
        // Special records: hair, stream and pre-shaded cluster ids, by the top two bits of the vis id.
        if (COV_SPECIAL != UNX_NONE)
        {
            const uint top = valid && specialElement < COV_CAP ? stream[e].x >> 30 : 0u;
            const uint kind = top == 2 ? COV_SPECIAL_HAIR : (top == 3 ? COV_SPECIAL_STREAM : (top == 1 ? COV_SPECIAL_PRESHADE : 0u));
            const uint n = WaveActiveCountBits(kind != 0), prefix = WavePrefixCountBits(kind != 0);
            uint base = 0;
            if (WaveIsFirstLane() && n > 0) state.InterlockedAdd(4 * VS_COV_SPECIAL, n, base);
            base = WaveReadLaneFirst(base);
            if (kind != 0)
            {
                const uint slot = base + prefix;
                if (slot < COV_SPECIAL_CAP)
                {
                    RWByteAddressBuffer special = ResourceDescriptorHeap[COV_SPECIAL];
                    special.Store2(4 * (COV_SPECIAL_HEADER + 2 * slot), uint2(specialElement, kind));
                }
                else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_COVERAGE_SPECIAL);
            }
        }
    }
#endif
}
#elif MODE == 7
groupshared uint gs_records[1024], gs_blocks[1024];

[numthreads(1024, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[COV_SCRATCH];
    const uint listed = min(state.Load(4 * VS_COV_TILES), COV_TILES), per = (listed + 1023) / 1024;
    const uint first = min(gi * per, listed), last = min(first + per, listed);
    uint records = 0, blocks = 0;
    for (uint i = first; i < last; ++i)
    {
        const uint tile = list.Load(4 * (COV_LIST_INFO + 4 * i));
        uint n = headers.Load(4 * (tile * COV_TILE_WORDS + COV_TILE_COUNT));
        if (n > COV_CAP)
        {
            state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: a tile holds at most the stored records
            n = COV_CAP;
        }
        records += n;
        blocks += (n + COV_BLOCK - 1) / COV_BLOCK;
    }
    gs_records[gi] = records;
    gs_blocks[gi] = blocks;
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 1; o < 1024; o <<= 1)  // inclusive scan (Hillis-Steele, 10 steps)
    {
        const uint r = gs_records[gi] + (gi >= o ? gs_records[gi - o] : 0), b = gs_blocks[gi] + (gi >= o ? gs_blocks[gi - o] : 0);
        GroupMemoryBarrierWithGroupSync();
        gs_records[gi] = r;
        gs_blocks[gi] = b;
        GroupMemoryBarrierWithGroupSync();
    }
    uint recordBase = gs_records[gi] - records, blockBase = gs_blocks[gi] - blocks;
    for (uint j = first; j < last; ++j)
    {
        const uint tile = list.Load(4 * (COV_LIST_INFO + 4 * j)), h = 4 * tile * COV_TILE_WORDS;
        const uint n = min(headers.Load(h + 4 * COV_TILE_COUNT), COV_CAP);
        list.Store4(4 * (COV_LIST_INFO + 4 * j), uint4(tile, n, recordBase, blockBase));
        headers.Store(h + 4 * COV_TILE_BASE, recordBase);
        uint heavy = 0;
        if (n > COV_BLOCK)
        {
            uint slot = 0;
            state.InterlockedAdd(4 * VS_COV_HEAVY, 1, slot);
            if (slot < COV_SCRATCH_SLOTS)
            {
                scratch.Store(4 * (COV_SCRATCH_SLOTS * COV_SCRATCH_WORDS + slot), j);
                heavy = slot + 1;
            }
            else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: the slots hold capacity / 1,025 tiles
        }
        headers.Store(h + 4 * COV_TILE_HEAVY, heavy);
        recordBase += n;
        blockBase += (n + COV_BLOCK - 1) / COV_BLOCK;
    }
    DeviceMemoryBarrierWithGroupSync();
    if (gi == 1023)
    {
        const uint totalBlocks = gs_blocks[1023], heavy = min(state.Load(4 * VS_COV_HEAVY), COV_SCRATCH_SLOTS);
        storeDispatch(list, COV_LIST_ARGS, listed);
        list.Store(4 * COV_LIST_COUNT, listed);
        list.Store(4 * COV_LIST_RECORDS, gs_records[1023]);
        list.Store(4 * COV_LIST_BLOCKS, totalBlocks);
        list.Store(4 * COV_LIST_POOL, COV_CAP);
        list.Store(4 * COV_LIST_TILES_X, COV_TILES_X);
        storeDispatch(list, COV_LIST_BLOCK_ARGS, totalBlocks);
        list.Store(4 * COV_LIST_HEAVY_COUNT, heavy);
        storeDispatch(list, COV_LIST_HEAVY_ARGS, heavy);
        list.Store(4 * 15, 0);
        state.Store(4 * VS_COV_BLOCKS, totalBlocks);
    }
}
#elif MODE == 9
groupshared uint gs_scan[COV_TILE_PIXELS];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint j = gid.x + gid.y * 65535u;
    if (j >= list.Load(4 * COV_LIST_COUNT)) return;  // uniform over the group
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[COV_COUNTERS];
    RWByteAddressBuffer starts = ResourceDescriptorHeap[COV_TILE_STARTS];
    const uint4 info = coverageTileInfo(list, j);
    const uint tile = info.x, key = tile * COV_TILE_PIXELS + lane;
    const uint c = counters.Load(4 * key);
    gs_scan[lane] = c;
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 1; o < COV_TILE_PIXELS; o <<= 1)  // inclusive scan (6 steps)
    {
        const uint v = gs_scan[lane] + (lane >= o ? gs_scan[lane - o] : 0);
        GroupMemoryBarrierWithGroupSync();
        gs_scan[lane] = v;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint start = gs_scan[lane] - c;
    if (lane == COV_TILE_PIXELS - 1 && gs_scan[lane] != info.y) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect
    starts.Store(4 * (j * COV_TILE_PIXELS + lane), start);
    counters.Store(4 * key, info.z + start + c);  // the pixel's absolute end: the scatter counts down from it
    const uint heavy = headers.Load(4 * (tile * COV_TILE_WORDS + COV_TILE_HEAVY));
    if (heavy != 0)
    {
        RWByteAddressBuffer scratch = ResourceDescriptorHeap[COV_SCRATCH];
        scratch.Store4(4 * ((heavy - 1) * COV_SCRATCH_WORDS + 4 * lane), uint4(0, 0xFFFFFFFFu, 0, 0xFFFFFFFFu));
    }
}
#elif MODE == 3
groupshared uint gs_union[COV_TILE_PIXELS], gs_farthestOpaque[COV_TILE_PIXELS], gs_nearest[COV_TILE_PIXELS], gs_farthest[COV_TILE_PIXELS];
groupshared uint gs_covered[2];

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint block = gid.x + gid.y * 65535u;
    if (block >= list.Load(4 * COV_LIST_BLOCKS)) return;  // uniform over the group
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[COV_RECORDS];
    const uint j = coverageBlockTile(list, block);
    const uint4 info = coverageTileInfo(list, j);
    const uint tile = info.x, n = info.y, first = (block - info.w) * COV_BLOCK;
    if (gi < COV_TILE_PIXELS)
    {
        gs_union[gi] = 0;
        gs_farthestOpaque[gi] = 0xFFFFFFFFu;
        gs_nearest[gi] = 0;
        gs_farthest[gi] = 0xFFFFFFFFu;
    }
    if (gi < 2) gs_covered[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint i = first + q * 256 + gi;
        if (i < n)
        {
            const uint4 r = records[info.z + i];
            const uint p = r.w >> 26, depth = r.y & ~COV_DEPTH_SEE_THROUGH;
            InterlockedMax(gs_nearest[p], depth);
            InterlockedMin(gs_farthest[p], depth);
            if ((r.y & COV_DEPTH_SEE_THROUGH) == 0 && r.z != 0)
            {
                InterlockedOr(gs_union[p], r.z);
                InterlockedMin(gs_farthestOpaque[p], r.y);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint heavy = headers.Load(4 * (tile * COV_TILE_WORDS + COV_TILE_HEAVY));  // uniform over the group
    if (heavy == 0)
    {
        if (gi < COV_TILE_PIXELS && finishPixel(tile, gi, gs_union[gi], gs_farthestOpaque[gi], gs_nearest[gi], gs_farthest[gi]))
            InterlockedOr(gs_covered[gi / 32], 1u << (gi % 32));
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0)
        {
            headers.Store(4 * (tile * COV_TILE_WORDS + COV_TILE_OPAQUE_LO), gs_covered[0]);
            headers.Store(4 * (tile * COV_TILE_WORDS + COV_TILE_OPAQUE_HI), gs_covered[1]);
        }
    }
    else if (gi < COV_TILE_PIXELS)
    {
        RWByteAddressBuffer scratch = ResourceDescriptorHeap[COV_SCRATCH];
        const uint w = 4 * ((heavy - 1) * COV_SCRATCH_WORDS + 4 * gi);
        if (gs_union[gi] != 0) scratch.InterlockedOr(w, gs_union[gi]);
        if (gs_farthestOpaque[gi] != 0xFFFFFFFFu) scratch.InterlockedMin(w + 4, gs_farthestOpaque[gi]);
        if (gs_nearest[gi] != 0) scratch.InterlockedMax(w + 8, gs_nearest[gi]);
        if (gs_farthest[gi] != 0xFFFFFFFFu) scratch.InterlockedMin(w + 12, gs_farthest[gi]);
    }
}
#elif MODE == 5
groupshared uint gs_covered[2];

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
    const uint slot = gid.x + gid.y * 65535u;
    if (slot >= list.Load(4 * COV_LIST_HEAVY_COUNT)) return;  // uniform over the group
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    RWByteAddressBuffer scratch = ResourceDescriptorHeap[COV_SCRATCH];
    const uint tile = coverageTileInfo(list, scratch.Load(4 * (COV_SCRATCH_SLOTS * COV_SCRATCH_WORDS + slot))).x;
    if (lane < 2) gs_covered[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint4 v = scratch.Load4(4 * (slot * COV_SCRATCH_WORDS + 4 * lane));
    if (finishPixel(tile, lane, v.x, v.y, v.z, v.w)) InterlockedOr(gs_covered[lane / 32], 1u << (lane % 32));
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        headers.Store(4 * (tile * COV_TILE_WORDS + COV_TILE_OPAQUE_LO), gs_covered[0]);
        headers.Store(4 * (tile * COV_TILE_WORDS + COV_TILE_OPAQUE_HI), gs_covered[1]);
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    const uint tile = gid.x + gid.y * COV_TILES_X;
    if (tile == 0 && lane < COV_LIST_INFO)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[COV_TILE_LIST];
        list.Store(4 * lane, 0);
    }
    clearTile(tile, lane);
}
#endif
