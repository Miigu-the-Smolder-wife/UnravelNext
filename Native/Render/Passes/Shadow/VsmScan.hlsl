// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// One-path page assignment (S request 20260926_S_vsm_one_path): every requested slot gets a physical page of this frame's
// atlas by a deterministic prefix sum over the slots in order (slot i before slot i + 1), so page numbers do not depend
// on thread timing; the page table is rewritten in full (unrequested slots 0) and every assigned page is drawn this frame.
//   MODE 0: per group of 1024 slots (256 threads x 4 consecutive slots) the number of requests -> groups[g].
//   MODE 1: one group: exclusive prefix over groups[] in place (the bases), totals, the page list count and the indirect
//           arguments of the per-page passes; requests past the capacity are counted (pool exhausted: the pool grows).
//   MODE 2: per group again: slot -> page = base + prefix; table entry (page | RESIDENT | DIRTY, tag), page list entry
//           (slot, page) at index page, page metadata; requests consumed.
// Sun page cache (VsmCache.hlsl, shadow.vsm.cache): requests marked kept (bit 31) keep their table entry and physical page
// (DIRTY and STALE cleared: not drawn); the others are counted and numbered as above, and the n-th one gets the n-th
// free physical page (P[3].x, VsmCache MODE 4: 0, 1, 2, .. when nothing is kept - the one path). The page list holds the
// pages drawn this frame, by that number. Stats: requested and allocated include the kept pages (the atlas sizing).
// P[0] = { requests UAV (raw), page table UAV (raw), groups UAV (raw), stats UAV (raw) }
// P[1] = { slots (scanned: the sun's and the assigned local lights'), capacity (atlas pages), VSM constants CBV, groups }
// P[2] = { page list UAV (raw: count, pad, (slot, page) pairs), page metadata UAV, local lights SRV, indirect args UAV }
// shadow.vsm.static_separate (VsmCache.hlsl): a kept request with VSM_REQ_DYNAMIC keeps its page and static copy and has its
// movable casters drawn anew - its table entry carries VSM_FLAG_DIRTY_DYNAMIC this frame and MODE 2 appends it to the
// dynamic page list (P[3].y; order by thread timing: each of its consumers works per page) and adds its group to the
// second dispatch of the per-page passes (args words 3..5).
// P[3] = { free pages (raw: count, 0, 0, 0, then pages), dynamic page list UAV (raw: count, pad, (slot, page) pairs;
//          0xFFFFFFFF: none), 0, 0 }
// Stats words: 0 requested, 1 assigned, 2 drawn (= assigned), 3 over capacity, 5 requested by pixels, 32 + k per level,
// 53 sampled 32^2 sub-tiles (shadow.vsm.subtile_stats).
#include "Passes/Shadow/VsmLocal.hlsli"

#define VSM_REQ_KEPT (1u << 31)
#define SCAN_THREADS 256u
#define SCAN_PER_THREAD 4u
#define SCAN_GROUP_SLOTS (SCAN_THREADS * SCAN_PER_THREAD)

#if MODE == 1
#define TOTAL_THREADS 1024u
groupshared uint g_sums[TOTAL_THREADS];
[numthreads(TOTAL_THREADS, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer groups = ResourceDescriptorHeap[P[0].z];
    const uint count = P[1].w, per = (count + TOTAL_THREADS - 1) / TOTAL_THREADS, first = lane * per;
    uint sum = 0;
    [loop] for (uint i = 0; i < per; ++i)
        if (first + i < count) sum += groups.Load((first + i) * 4);
    g_sums[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    // Inclusive scan over the 1024 partial sums (log2 steps).
    [loop] for (uint d = 1; d < TOTAL_THREADS; d <<= 1)
    {
        const uint v = lane >= d ? g_sums[lane - d] : 0;
        GroupMemoryBarrierWithGroupSync();
        g_sums[lane] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint base = g_sums[lane] - sum;
    [loop] for (uint j = 0; j < per; ++j)
        if (first + j < count)
        {
            const uint n = groups.Load((first + j) * 4);
            groups.Store((first + j) * 4, base);
            base += n;
        }
    if (lane == TOTAL_THREADS - 1)
    {
        RWByteAddressBuffer free = ResourceDescriptorHeap[P[3].x];
        const uint total = g_sums[lane], capacity = min(P[1].y, free.Load(0)), assigned = min(total, capacity);
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
        const uint kept = stats.Load(24);  // (VsmCache MODE 3)
        stats.Store4(0, uint4(total + kept, assigned + kept, assigned, total - assigned));
        stats.Store(16, capacity);  // free physical pages for the new ones
        RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
        list.Store2(0, uint2(assigned, 0));
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[2].w];
        args.Store3(0, uint3(assigned, 1, 1));
        args.Store3(12, uint3(0, 1, 1));  // the dynamic page list's dispatch: MODE 2 adds a group per page
        if (P[3].y != 0xFFFFFFFFu)
        {
            RWByteAddressBuffer dynamicList = ResourceDescriptorHeap[P[3].y];
            dynamicList.Store2(0, uint2(0, 0));
        }
    }
}
#else
groupshared uint g_waveSums[SCAN_THREADS];

[numthreads(SCAN_THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint group = gid.y * 65535u + gid.x;
    if (group >= P[1].w) return;  // uniform per group
    const uint slots = P[1].x, first = group * SCAN_GROUP_SLOTS + lane * SCAN_PER_THREAD;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    uint req[SCAN_PER_THREAD];
    uint mine = 0;
    [unroll] for (uint i = 0; i < SCAN_PER_THREAD; ++i)
    {
        req[i] = first + i < slots ? requests.Load((first + i) * 4) : 0u;
        mine += req[i] != 0 && (req[i] & VSM_REQ_KEPT) == 0 ? 1u : 0u;  // (kept requests take no new page)
    }
    // Exclusive prefix of 'mine' over the group's threads in thread order, and the group total.
    const uint laneCount = WaveGetLaneCount(), wave = lane / laneCount, waves = (SCAN_THREADS + laneCount - 1) / laneCount;
    const uint inWave = WavePrefixSum(mine), waveSum = WaveActiveSum(mine);
    if (WaveIsFirstLane()) g_waveSums[wave] = waveSum;
    GroupMemoryBarrierWithGroupSync();
    uint before = inWave, total = 0;
    [loop] for (uint w = 0; w < waves; ++w)
    {
        if (w < wave) before += g_waveSums[w];
        total += g_waveSums[w];
    }
    RWByteAddressBuffer groups = ResourceDescriptorHeap[P[0].z];
#if MODE == 0
    if (lane == 0) groups.Store(group * 4, total);
#else
    RWByteAddressBuffer free = ResourceDescriptorHeap[P[3].x];
    const uint capacity = min(P[1].y, free.Load(0));
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].x];
    RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[2].y];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].z];
    uint page = groups.Load(group * 4) + before;
    [unroll] for (uint j = 0; j < SCAN_PER_THREAD; ++j)
    {
        const uint slot = first + j;
        if (slot >= slots) continue;
        uint2 e = uint2(0, 0);
        if (req[j] != 0)
        {
            requests.Store(slot * 4, 0);
            if (req[j] & VSM_REQ_PIXEL) stats.InterlockedAdd(20, 1);
            uint tag;
            if (slot < VSM_SUN_SLOTS)
            {
                const uint k = slot / VSM_SLOTS_PER_LEVEL;
                tag = vsmTag(vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k));
                stats.InterlockedAdd(128 + k * 4, 1);
                if (req[j] >> 16) stats.InterlockedAdd(212, countbits(req[j] >> 16));
            }
            else
            {
                StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[2].z];
                tag = lights[(slot - VSM_SUN_SLOTS) / VSM_LOCAL_LIGHT_SLOTS].generation;
            }
            if (req[j] & VSM_REQ_KEPT)
            {
                // the page as drawn in an earlier frame: same physical page, tag and metadata
                const uint2 last = table.Load2(slot * 8);
                e = uint2(last.x & ~(VSM_FLAG_DIRTY | VSM_FLAG_DIRTY_DYNAMIC | VSM_FLAG_STALE | VSM_FLAG_STALE_DYNAMIC), last.y);
                meta[last.x & VSM_PHYS_MASK].lastRequested = c.frame;
                if ((req[j] & VSM_REQ_DYNAMIC) != 0 && P[3].y != 0xFFFFFFFFu)
                {
                    // its movable casters are drawn anew over its static copy (each kept page appears once: at most the
                    // atlas's pages, the list's size)
                    e.x |= VSM_FLAG_DIRTY_DYNAMIC;
                    RWByteAddressBuffer dynamicList = ResourceDescriptorHeap[P[3].y];
                    uint at;
                    dynamicList.InterlockedAdd(0, 1, at);
                    dynamicList.Store2(8 + at * 8, uint2(slot, last.x & VSM_PHYS_MASK));
                    RWByteAddressBuffer args = ResourceDescriptorHeap[P[2].w];
                    args.InterlockedAdd(12, 1);
                    stats.InterlockedAdd(4 * 71, 1);
                    meta[last.x & VSM_PHYS_MASK].renderTime = asuint(c.time);
                    meta[last.x & VSM_PHYS_MASK].maxHeight = VSM_EMPTY;  // VsmPageMax
                }
            }
            else
            {
                // 'page' is the request's number among this frame's new ones: the page-th free physical page
                if (page < capacity)
                {
                    const uint phys = free.Load(16 + page * 4);
                    e = uint2(phys | VSM_FLAG_RESIDENT | VSM_FLAG_DIRTY, tag);
                    list.Store2(8 + page * 8, uint2(slot, phys));
                    VsmPageMeta m = (VsmPageMeta)0;
                    m.owner = slot | 0x80000000u;
                    m.lastRequested = c.frame;
                    m.renderTime = asuint(c.time);
                    m.maxHeight = VSM_EMPTY;  // VsmPageMax
                    meta[phys] = m;
                }
                ++page;
            }
        }
        table.Store2(slot * 8, e);
    }
#endif
}
#endif
