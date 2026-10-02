// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5
// Depth buckets of the coverage raster (CoverageBuckets.hlsli: layout, root constants and why the result is exact): the
// band B list sorted nearer first into buckets, and the cover the buckets leave for the ones after them. Every pass does
// a fixed amount of work per group (INTERFACES 3.6):
//   MODE=0 begin (64 threads per group over the header's words): the bins' header and the dispatch arguments emptied;
//          the arguments over the band B list (both cull phases), 64 entries per group.
//   MODE=1 classify (64 list entries per group): the entry's fine bin from the nearest distance of its cluster's world
//          sphere - a skinned or view-model cluster, whose sphere does not bound what is drawn, goes to the nearest bin
//          untested; an entry whose sphere lies behind band A (the final HiZ over its rectangle, grown by the pixel
//          kernel's neighbourhood) gets no bin: every fragment of it would be dropped (the cull kernels tested phase 1's
//          clusters against last frame's HiZ only, phase 2's against the HiZ before phase 2's own raster).
//   MODE=2 prefix (1 thread, COVB_FINE bins): the bins' first sorted entries (their write cursors), the buckets - cut
//          where the running count reaches each bucket's share of the entries, so they hold equal counts as far as the
//          bins allow - and the raster arguments of every bucket.
//   MODE=3 scatter (64 list entries per group): each entry with a bin to the bin's next sorted slot.
//   MODE=4 cover arguments (1 thread, after a bucket's raster): the stream entries that bucket appended, COV_BLOCK per
//          group.
//   MODE=5 cover (COV_BLOCK stream entries per group): each stored opaque fragment with a subsample into its pixel's
//          cover { union, ~farthest } and its tile's header words (pixels with a full union, ~farthest). Atomics only
//          where a plain load says the value changes (the union gains bits, the complement of the depth grows); the
//          atomic that fills a pixel's union counts the pixel (one such atomic per pixel: the value only gains bits).
#include "Passes/Visibility/CoverageBuckets.hlsli"

void storeDispatch(RWByteAddressBuffer args, uint word, uint groups)
{
    args.Store3(4 * word, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1));
}

#if MODE == 0
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_BIN_ARGS];
    if (id.x < COVB_HEADER) bins.Store(4 * id.x, 0);
    if (id.x < COVB_ARG_WORDS)
    {
        const uint entries = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), COV_LIST_CAPACITY), groups = (entries + 63) / 64;
        const uint3 grid = uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1);
        args.Store(4 * id.x, id.x < COVB_ARG_ENTRIES + 3 ? grid[min(id.x, 2u)] : 0u);  // (COVB_ARG_ENTRIES is word 0)
    }
}

#elif MODE == 1 || MODE == 3
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    ByteAddressBuffer lists = ResourceDescriptorHeap[COV_LISTS];
    const uint capacity = COV_LIST_CAPACITY;
    const uint entries = min(state.Load(4 * (VS_LIST_COUNT + LIST_B)), capacity);
    const uint e = (gid.x + gid.y * 65535u) * 64 + lane;
    const bool active = e < entries;
#if MODE == 1
    uint bin = COVB_NO_BIN;
    bool behind = false;
    if (active)
    {
        const uint listEntry = lists.Load(4 * (LIST_B * capacity + e));
        StructuredBuffer<uint2> visible = ResourceDescriptorHeap[COV_VISIBLE];
        const uint2 entry = visible[listEntry & ~LIST_ENTRY_MIXED];
        StructuredBuffer<CullView> views = ResourceDescriptorHeap[COV_VIEWS];
        const CullView v = views[entry.y >> 24];
        const GpuInstance inst = loadInstance(entry.x);
        const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
        bin = 0;
        if ((inst.flags & (INSTANCE_SKINNED | INSTANCE_VIEW_MODEL)) == 0)
        {
            const float4 s = worldSphere(inst, inst.objectToWorld, cl.boundsSphere);
            const float distance = (v.orthographic ? dot(s.xyz - v.position, v.viewDirection.xyz) : length(s.xyz - v.position)) - s.w;
            bin = covbFineBin(distance);
            float4 rect;
            float nearest;
            if (projectSphere(v.viewProj, v.viewportSize, s, rect, nearest) && coverageRectBehindBandA(rect.xy, rect.zw, nearest))
            {
                bin = COVB_NO_BIN;
                behind = true;
            }
        }
        bins.Store(4 * covbBinOf(capacity, e), bin);
        if (bin != COVB_NO_BIN) bins.InterlockedAdd(4 * (COVB_HISTOGRAM + bin), 1);
    }
    const uint behindCount = WaveActiveCountBits(behind);
    if (behindCount > 0 && WaveIsFirstLane()) state.InterlockedAdd(4 * VS_COV_CLUSTERS_HIZ, behindCount);
#else
    if (!active) return;
    const uint bin = bins.Load(4 * covbBinOf(capacity, e));
    if (bin >= COVB_FINE) return;  // not drawn
    uint slot = 0;
    bins.InterlockedAdd(4 * (COVB_HISTOGRAM + bin), 1, slot);
    if (slot < capacity) bins.Store(4 * covbSorted(capacity, slot), lists.Load(4 * (LIST_B * capacity + e)));
    else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: the cursors partition the kept entries
#endif
}

#elif MODE == 2
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_BIN_ARGS];
    const uint buckets = clamp(COV_BUCKETS, 1u, COVB_BUCKETS_MAX);
    uint total = 0;
    for (uint f = 0; f < COVB_FINE; ++f) total += bins.Load(4 * (COVB_HISTOGRAM + f));
    uint running = 0, first = 0, bucket = 0;
    for (uint g = 0; g < COVB_FINE; ++g)
    {
        const uint n = bins.Load(4 * (COVB_HISTOGRAM + g));
        bins.Store(4 * (COVB_HISTOGRAM + g), running);  // the bin's write cursor
        running += n;
        // a bucket ends with the bin that brings the running count to its share (a bin holding several shares closes
        // several buckets: the later ones are empty). At most 'buckets' closings over the whole loop.
        while (bucket + 1 < buckets && running * buckets >= total * (bucket + 1))
        {
            bins.Store(4 * (COVB_FIRST + bucket), first);
            bins.Store(4 * (COVB_COUNT + bucket), running - first);
            first = running;
            ++bucket;
        }
    }
    bins.Store(4 * (COVB_FIRST + bucket), first);  // the last bucket: what is left
    bins.Store(4 * (COVB_COUNT + bucket), total - first);
    bins.Store(4 * COVB_ENTRIES, total);
    for (uint k = 0; k < COVB_BUCKETS_MAX; ++k) storeDispatch(args, COVB_ARG_DRAW + 3 * k, k < buckets ? bins.Load(4 * (COVB_COUNT + k)) : 0u);
}

#elif MODE == 4
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    RWByteAddressBuffer args = ResourceDescriptorHeap[COV_BIN_ARGS];
    const uint begin = bins.Load(4 * COVB_COVER_END), end = max(min(state.Load(4 * VS_COV_FRAGMENTS), COV_CAP), begin);
    bins.Store(4 * COVB_COVER_BEGIN, begin);
    bins.Store(4 * COVB_COVER_END, end);
    storeDispatch(args, COVB_ARG_COVER, (end - begin + COV_BLOCK - 1) / COV_BLOCK);
}

#else
[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer bins = ResourceDescriptorHeap[COV_BINS];
    RWStructuredBuffer<uint4> stream = ResourceDescriptorHeap[COV_STREAM];
    RWByteAddressBuffer keys = ResourceDescriptorHeap[COV_KEYS];
    RWByteAddressBuffer cover = ResourceDescriptorHeap[COV_COVER];
    RWByteAddressBuffer headers = ResourceDescriptorHeap[COV_TILE_HEADERS];
    const uint begin = bins.Load(4 * COVB_COVER_BEGIN), end = bins.Load(4 * COVB_COVER_END);
    const uint first = begin + (gid.x + gid.y * 65535u) * COV_BLOCK;
    if (first >= end) return;  // uniform over the group
    const uint keyEnd = COV_TILES * COV_TILE_PIXELS;
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint e = first + q * 256 + gi;
        const uint key = e < end ? keys.Load(4 * e) : 0xFFFFFFFFu;  // (a key outside the view: a defect the count pass reports)
        uint4 r = 0;
        if (key < keyEnd) r = stream[e];
        // as the block pass: opaque, with a subsample
        if (key < keyEnd && (r.y & COV_DEPTH_SEE_THROUGH) == 0 && r.z != 0)
        {
            const uint complement = ~r.y;  // the farthest fragment = the smallest depth bits = the largest complement
            const uint tileWords = 4 * ((key / COV_TILE_PIXELS) * COV_TILE_WORDS);
            if (headers.Load(tileWords + 4 * COV_TILE_NEAR) < complement) headers.InterlockedMax(tileWords + 4 * COV_TILE_NEAR, complement);
            if (cover.Load(8 * key + 4) < complement) cover.InterlockedMax(8 * key + 4, complement);
            if ((cover.Load(8 * key) & r.z) != r.z)
            {
                uint before = 0;
                cover.InterlockedOr(8 * key, r.z, before);
                if (before != COV_MASK_FULL && (before | r.z) == COV_MASK_FULL) headers.InterlockedAdd(tileWords + 4 * COV_TILE_FULL, 1);
            }
        }
    }
}
#endif
