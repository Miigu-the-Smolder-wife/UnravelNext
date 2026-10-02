// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// Raster bins of a full view's band A lists (visibility.raster_depth_sort, visibility.software_raster; the reference's
// NaniteRasterBinning: its bins are the raster pipeline - here the four band A lists, back-face culled or not, alpha
// tested or not - and the software / hardware split, its depth buckets the order inside a bin). After a cull phase's
// draw arguments, the entries that phase added to each band A list are sorted into a copy of the lists (the same
// layout: list x capacity + entry) by a counting sort over 512 keys per list:
//   key = fine depth bin (256, log2 of the cluster sphere's nearest distance, nearer first)
//         + 256 for a cluster the software rasteriser draws
// so the mesh raster draws each list's hardware entries front to back - the depth test rejects what nearer clusters of
// the same draw already cover before its pixel kernel and its writes - and the software entries are one run after
// them (LIST_ENTRY_SOFTWARE set), dispatched to VisRasterSw.hlsl. The depth buffer and the vis ids are the same either
// way up to the order of equal depths; nothing here culls.
// A cluster is the software rasteriser's when (P[10].z = the largest rectangle in pixels, 0: none) its list has no
// alpha test, it is not a mixed sheet cluster, its instance is not skinned, a view model or a patched terrain tile (the
// cluster sphere bounds what is drawn, no triangle is dropped), the view is a perspective view without a clip plane,
// and the sphere's box lies in front of the near plane with a rectangle of at most that many pixels a side.
//   MODE=0 begin (64 threads over the header's words): the header emptied; the arguments over the phase's entries of
//          the four lists together (64 per group).
//   MODE=1 classify (64 entries per group): the entry's key stored and counted.
//   MODE=2 prefix (1 thread, 4 x 512 keys): each key's first sorted place; per list the hardware and software counts and
//          their arguments (one mesh group / one compute group per entry).
//   MODE=3 scatter (64 entries per group): each entry to its key's next place.
// Root constants: the cull kernels' (CullShared.hlsli; LISTS_UAV the run's lists, read), and
//   P[10] bins UAV (raw: header, then a key per entry), bin arguments UAV (raw), software rectangle limit (float), sorted
//        lists UAV (raw)
#include "Passes/Visibility/CullShared.hlsli"

#define BINS_UAV P[10].x
#define BIN_ARGS_UAV P[10].y
#define SW_LIMIT_PX asfloat(P[10].z)
#define SORTED_UAV P[10].w

#define RB_LISTS VS_A_LISTS
#define RB_DEPTH_KEYS 256u
#define RB_KEYS 512u           // per list: depth x { hardware, software }
#define RB_HW_COUNT 0u         // + list: entries the mesh raster draws (sorted first)
#define RB_SW_COUNT 4u         // + list: entries the software rasteriser draws (after them)
#define RB_FIRST 8u            // + list: the phase's first entry of the list
#define RB_COUNT 12u           // + list: the phase's entries of the list
#define RB_HISTOGRAM 16u       // + list x RB_KEYS + key: entries; from the prefix on, the key's next sorted place
#define RB_HEADER (RB_HISTOGRAM + RB_LISTS * RB_KEYS)
#define RB_ARG_ENTRIES 0u      // 0..2 DispatchIndirect over the phase's entries of the four lists (64 per group)
#define RB_ARG_HW 4u           // + 3 x list: DispatchMesh over the list's hardware entries
#define RB_ARG_SW 16u          // + 3 x list: DispatchIndirect over its software entries (one group each)
#define RB_ARG_WORDS 32u

void storeDispatch(RWByteAddressBuffer args, uint word, uint groups)
{
    args.Store3(4 * word, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1));
}

// The phase's entries of list l: [first, first + count).
uint2 phaseRange(RWByteAddressBuffer state, uint l)
{
    const uint end = min(state.Load(4 * (VS_LIST_COUNT + l)), CAP_VISIBLE);
    const uint first = CULL_PHASE == 1 ? 0u : min(state.Load(4 * (VS_LIST_PHASE1 + l)), end);
    return uint2(first, end - first);
}

// Combined entry e of the four lists -> (list, entry in the list); false past the last.
bool combinedEntry(RWByteAddressBuffer state, uint e, out uint list, out uint entry)
{
    list = 0;
    entry = 0;
    uint before = 0;
    [unroll] for (uint l = 0; l < RB_LISTS; ++l)
    {
        const uint2 range = phaseRange(state, l);
        if (e >= before && e < before + range.y)
        {
            list = l;
            entry = range.x + (e - before);
            return true;
        }
        before += range.y;
    }
    return false;
}

#if MODE == 0
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[BINS_UAV];
    RWByteAddressBuffer args = ResourceDescriptorHeap[BIN_ARGS_UAV];
    if (id.x < RB_HEADER) bins.Store(4 * id.x, 0);
    if (id.x < RB_ARG_WORDS)
    {
        uint entries = 0;
        [unroll] for (uint l = 0; l < RB_LISTS; ++l) entries += phaseRange(state, l).y;
        const uint groups = (entries + 63) / 64;
        const uint3 grid = uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1);
        args.Store(4 * id.x, id.x < 3 ? grid[min(id.x, 2u)] : 0u);  // (RB_ARG_ENTRIES is word 0)
    }
}

#elif MODE == 1 || MODE == 3
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[BINS_UAV];
    RWByteAddressBuffer lists = ResourceDescriptorHeap[LISTS_UAV];
    const uint e = (gid.x + gid.y * 65535u) * 64 + lane;
    uint list, entry;
    if (!combinedEntry(state, e, list, entry)) return;
    const uint listEntry = lists.Load(4 * (list * CAP_VISIBLE + entry));
#if MODE == 1
    RWStructuredBuffer<uint2> visible = ResourceDescriptorHeap[VISIBLE_UAV];
    const uint2 item = visible[listEntry & ~LIST_ENTRY_FLAGS];
    const CullView v = loadView(item.y >> 24);
    const GpuInstance inst = loadInstance(item.x);
    const GpuCluster cl = loadCluster(item.y & 0xFFFFFFu);
    const float4 s = worldSphere(inst, inst.objectToWorld, cl.boundsSphere);
    const float distance = (v.orthographic ? dot(s.xyz - v.position, v.viewDirection.xyz) : length(s.xyz - v.position)) - s.w;
    // 12.8 bins per octave from 1/16 m: nearer = smaller
    uint key = (uint)clamp(log2(max(distance, 0.0625) / 0.0625) * (RB_DEPTH_KEYS / 20.0), 0.0, RB_DEPTH_KEYS - 1.0);
    const float limit = SW_LIMIT_PX;
    const bool plain = (list == LIST_A_BACK || list == LIST_A_NONE) && (listEntry & LIST_ENTRY_MIXED) == 0 &&
                       (inst.flags & (INSTANCE_SKINNED | INSTANCE_VIEW_MODEL)) == 0 && inst.patch == UNX_NONE;
    if (limit > 0 && plain && v.orthographic == 0 && !any(v.clipPlane != 0))
    {
        float4 rect;
        float nearest;
        if (projectSphere(v.viewProj, v.viewportSize, s, rect, nearest) && max(rect.z - rect.x, rect.w - rect.y) <= limit) key += RB_DEPTH_KEYS;
    }
    bins.Store(4 * (RB_HEADER + e), key);
    bins.InterlockedAdd(4 * (RB_HISTOGRAM + list * RB_KEYS + key), 1);
#else
    RWByteAddressBuffer sorted = ResourceDescriptorHeap[SORTED_UAV];
    const uint key = min(bins.Load(4 * (RB_HEADER + e)), RB_KEYS - 1);
    uint place = 0;
    bins.InterlockedAdd(4 * (RB_HISTOGRAM + list * RB_KEYS + key), 1, place);
    if (place < CAP_VISIBLE) sorted.Store(4 * (list * CAP_VISIBLE + place), listEntry | (key >= RB_DEPTH_KEYS ? LIST_ENTRY_SOFTWARE : 0u));
    else state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);  // a defect: the places partition the phase's entries
#endif
}

#else
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer bins = ResourceDescriptorHeap[BINS_UAV];
    RWByteAddressBuffer args = ResourceDescriptorHeap[BIN_ARGS_UAV];
    for (uint l = 0; l < RB_LISTS; ++l)
    {
        const uint2 range = phaseRange(state, l);
        uint running = 0, hardware = 0;
        for (uint k = 0; k < RB_KEYS; ++k)  // (hardware keys first: the software entries follow them)
        {
            const uint word = 4 * (RB_HISTOGRAM + l * RB_KEYS + k);
            const uint n = bins.Load(word);
            bins.Store(word, range.x + running);
            running += n;
            if (k == RB_DEPTH_KEYS - 1) hardware = running;
        }
        bins.Store(4 * (RB_HW_COUNT + l), hardware);
        bins.Store(4 * (RB_SW_COUNT + l), running - hardware);
        bins.Store(4 * (RB_FIRST + l), range.x);
        bins.Store(4 * (RB_COUNT + l), range.y);
        storeDispatch(args, RB_ARG_HW + 3 * l, hardware);
        storeDispatch(args, RB_ARG_SW + 3 * l, running - hardware);
    }
}
#endif
