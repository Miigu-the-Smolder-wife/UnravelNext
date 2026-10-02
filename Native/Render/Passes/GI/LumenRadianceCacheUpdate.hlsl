// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5,6,7,8,9
// r.gi.rc.* (LumenRadianceCache.hlsli): the radiance cache's bookkeeping passes.
//   MODE 0 clear     per cell: indirection = invalid; thread 0 zeroes the frame's counters and histogram.
//   MODE 1 mark      per 16 x 16 px screen tile: the surface under one jittered pixel marks its 8 probe cells (the
//                    module's own marker, at the screen probes' spacing; consumers may mark more).
//   MODE 2 reuse     per probe slot: a probe allocated last frame finds its cell in this frame's clipmap (the clipmaps
//                    scroll in whole cells, the probe's world position is fixed); it keeps its slot when the cell is
//                    marked or it was used within keepFrames frames (2 when free slots are short), else it is freed.
//   MODE 3 allocate  per cell: a marked cell without a probe takes a slot (the free list first), never traced; every
//                    probe's priority bucket and trace cost go into the histogram.
//   MODE 4 select    one thread: clamps the allocators; the last bucket that fits the budget and the cost left in it.
//   MODE 5 traces    per probe slot: probes in buckets up to the selected one are queued for tracing (new probes always;
//                    past the budget they are traced at a quarter of the rays).
//   MODE 6 finish    one thread: the trace count (at most the capacity) into the ray pass's dispatch descriptions and
//                    the filter passes' dispatch arguments, one of each per chunk of probes: a dispatch holds at most
//                    P[2].w probes (the structural bound of one dispatch's work; the chunks past the count are empty).
//   MODE 7 reset     per probe slot: empties the cache (a new scene, a size change, an origin shift).
//   MODE 8 validate  per cell: a probe that has never been traced is not offered to readers (indirection = invalid;
//                    it keeps its slot).
//   MODE 9 hit marks per position of the list the last frame's ray hits wrote (lrcHitMark; at most LRC_HIT_MARKS): its
//                    8 probe cells are marked, at a clipmap P[1].w levels coarser than the position's own (such hits
//                    fall anywhere a ray reaches: at the surfaces' probe density they would take the cache's budget).
//                    P[2].x != 0: instead, one thread empties the list for this frame's hits.
// Probe slot (raw, 16 B): coord x | y << 8 | z << 16 | clipmap << 24 | valid << 31; last used frame; last traced frame
// (0: never); bucket | trace cost << 8.
// State (raw): 0 slots allocated, 4 free list count, 8 traces queued, 12 selected bucket, 16 cost allowed from it,
// 20 cost taken from it, 24 new probes' cost (at the downsampled cost, then growing), 32.. histogram (16 words).
// Trace record (raw, 16 B): probe centre (world), clipmap << 24 | slot | force downsample << 31.
// P[0] = { parameters SRV (LrcParams), indirection UAV, probe slots UAV, state UAV }
// P[1] = { free list UAV, traces UAV, depth SRV (MODE 1) / the hit-mark list UAV (MODE 9), tile px (MODE 1) / clipmap
//          bias (MODE 9) }
// P[2] = { ray dispatch descriptions UAV, offset of a description's Width, filter dispatch arguments UAV, probes per
//          dispatch } (MODE 6), P[3] = { chunks, stride of a ray dispatch description (bytes), 0, 0 }: chunk c's
//          description at c x stride; its filter arguments at 32 c (filter) and 32 c + 16 (store).
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/GI/LumenRadianceCacheMark.hlsli"

#define LRC_COST_DOWN 1u
#define LRC_COST_NORMAL 4u

uint lrcPackSlot(uint3 coord, uint clipmap) { return coord.x | (coord.y << 8) | (coord.z << 16) | (clipmap << 24) | 0x80000000u; }

uint lrcBucket(uint lastTraced, uint lastUsed, uint clipmap)
{
    if (lastTraced == LRC_NEVER_TRACED) return 0;
    const float between = lastUsed > lastTraced ? float(lastUsed - lastTraced) : 1.0;
    return LRC_HISTOGRAM - 1 - uint(clamp(log2(between / (clipmap + 1.0)), 0.0, float(LRC_HISTOGRAM - 2)));
}
uint lrcCost(LrcParams p, float3 centre) { return distance(centre, g_cameraPosition) >= p.downsampleDistance ? LRC_COST_DOWN : LRC_COST_NORMAL; }

#if MODE == 0
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
    if (id.x < p.grid * p.clipmaps && id.y < p.grid && id.z < p.grid) indirection[id] = LRC_INVALID;
    if (all(id == 0))
    {
        RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
        for (uint i = 8; i < 32 + 4 * LRC_HISTOGRAM; i += 4) state.Store(i, 0);
    }
}
#elif MODE == 1
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint tilePx = P[1].w;
    const uint2 size = uint2(g_viewWidth, g_viewHeight);
    if (any(id.xy * tilePx >= size)) return;
    const LrcParams p = lrcParams(P[0].x);
    const uint h = (id.x * 73856093u) ^ (id.y * 19349663u) ^ (g_frameIndex * 83492791u);
    const uint2 pixel = min(id.xy * tilePx + uint2((h >> 4) % tilePx, (h >> 12) % tilePx), size - 1);
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[1].z];
    const float device = depthTex[pixel];
    if (!(device > 0)) return;
    const float2 ndc = float2((pixel.x + 0.5) / size.x * 2 - 1, 1 - (pixel.y + 0.5) / size.y * 2);
    const float4 world = mul(g_invViewProj, float4(ndc, device, 1));
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
    lrcMark(indirection, p, world.xyz / world.w, ((h >> 20) & 1023u) / 1024.0 * 0.999 + 0.0005);
}
#elif MODE == 2
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].z];
    const uint slot = id.x;
    if (slot >= min(state.Load(0), p.maxProbes)) return;
    const uint4 s = slots.Load4(slot * 16);
    if ((s.x & 0x80000000u) == 0) return;
    const uint clipmap = (s.x >> 24) & 0x7u;
    const float3 centre = p.prevCornerCell[clipmap].xyz + (float3(s.x & 0xFFu, (s.x >> 8) & 0xFFu, (s.x >> 16) & 0xFFu) + 0.5) * p.prevCornerCell[clipmap].w;
    const int3 coord = int3(floor(lrcCoordFloat(p, centre, clipmap)));
    bool kept = false;
    if (all(coord >= 0) && all(coord < int(p.grid)))
    {
        RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
        const uint3 cell = uint3(coord.x + int(clipmap * p.grid), coord.y, coord.z);
        const uint marker = indirection[cell];
        // (few free slots: unused probes go after 2 frames, so that this frame's new probes find slots)
        const uint freeSlots = p.maxProbes - min(state.Load(0), p.maxProbes) + state.Load(4);
        const uint keepFrames = freeSlots < p.budget ? min(2u, p.keepFrames) : p.keepFrames;
        if (marker == LRC_USED || p.frame - s.y < keepFrames)
        {
            kept = true;
            slots.Store(slot * 16, lrcPackSlot(uint3(coord), clipmap));
            if (marker == LRC_USED) slots.Store(slot * 16 + 4, p.frame);
            indirection[cell] = slot;
        }
    }
    if (!kept)
    {
        RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[1].x];
        uint at;
        state.InterlockedAdd(4, 1u, at);
        freeList.Store(4 * at, slot);
        slots.Store4(slot * 16, uint4(0, 0, 0, 0));
    }
}
#elif MODE == 3
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    const uint clipmap = id.x / p.grid;
    const uint3 coord = uint3(id.x - clipmap * p.grid, id.y, id.z);
    if (clipmap >= p.clipmaps || coord.y >= p.grid || coord.z >= p.grid) return;
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
    const uint e = indirection[id];
    if (e == LRC_INVALID) return;
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].z];
    const uint cost = lrcCost(p, lrcProbePosition(p, coord, clipmap));
    uint slot = e, bucket = 0;
    if (e == LRC_USED)
    {
        // a marked cell without a probe: a slot from the free list, else a new one
        uint freeCount;
        state.InterlockedAdd(4, 0xFFFFFFFFu, freeCount);
        if (int(freeCount) > 0)
        {
            RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[1].x];
            slot = freeList.Load(4 * (freeCount - 1));
        }
        else state.InterlockedAdd(0, 1u, slot);
        if (slot >= p.maxProbes)
        {
            indirection[id] = LRC_INVALID;  // the atlas is full: the cell stays without a probe this frame
            return;
        }
        slots.Store4(slot * 16, uint4(lrcPackSlot(coord, clipmap), p.frame, LRC_NEVER_TRACED, cost << 8));
        indirection[id] = slot;
        state.InterlockedAdd(24, LRC_COST_DOWN);
    }
    else
    {
        const uint2 used = slots.Load2(slot * 16 + 4);
        bucket = lrcBucket(used.y, used.x, clipmap);
        slots.Store(slot * 16 + 12, bucket | (cost << 8));
    }
    state.InterlockedAdd(32 + 4 * bucket, cost);
}
#elif MODE == 4
[numthreads(1, 1, 1)]
void main()
{
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    state.Store(0, min(state.Load(0), p.maxProbes));
    state.Store(4, uint(max(int(state.Load(4)), 0)));
    const uint budget = p.budget * LRC_COST_NORMAL;
    uint sum = 0, bucket = 0, fromBucket = budget;
    for (; bucket < LRC_HISTOGRAM; ++bucket)
    {
        const uint inBucket = state.Load(32 + 4 * bucket);
        if (sum + inBucket >= budget)
        {
            fromBucket = budget - sum;
            break;
        }
        sum += inBucket;
    }
    state.Store(12, bucket);
    state.Store(16, fromBucket);
}
#elif MODE == 5
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].z];
    const uint slot = id.x;
    if (slot >= state.Load(0)) return;
    const uint4 s = slots.Load4(slot * 16);
    if ((s.x & 0x80000000u) == 0) return;
    const uint bucket = s.w & 0xFFu, cost = (s.w >> 8) & 0xFFu, clipmap = (s.x >> 24) & 0x7u;
    const uint maxBucket = state.Load(12);
    if (bucket > maxBucket) return;
    bool forceDown = false;
    if (bucket == maxBucket && maxBucket > 0)
    {
        uint taken;
        state.InterlockedAdd(20, cost, taken);
        if (taken + cost > state.Load(16)) return;
    }
    else if (bucket == 0)
    {
        // new probes are always traced; past the budget at the downsampled resolution
        uint before;
        state.InterlockedAdd(24, cost - LRC_COST_DOWN, before);
        forceDown = before + cost - LRC_COST_DOWN > p.budget * LRC_COST_NORMAL;
    }
    uint at;
    state.InterlockedAdd(8, 1u, at);
    if (at >= p.traceCapacity) return;  // (stays untraced: first in line next frame)
    RWByteAddressBuffer traces = ResourceDescriptorHeap[P[1].y];
    const float3 centre = lrcProbePosition(p, uint3(s.x & 0xFFu, (s.x >> 8) & 0xFFu, (s.x >> 16) & 0xFFu), clipmap);
    traces.Store4(at * 16, uint4(asuint(centre), (clipmap << 24) | slot | (forceDown ? 0x80000000u : 0u)));
    slots.Store(slot * 16 + 8, p.frame);
}
#elif MODE == 6
[numthreads(1, 1, 1)]
void main()
{
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
    const uint count = min(state.Load(8), p.traceCapacity);
    state.Store(8, count);
    RWByteAddressBuffer rayDesc = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer filterArgs = ResourceDescriptorHeap[P[2].z];
    const uint perDispatch = max(P[2].w, 1u);
    for (uint c = 0; c < P[3].x; ++c)
    {
        const uint n = count > c * perDispatch ? min(count - c * perDispatch, perDispatch) : 0;
        // (an empty chunk: every dimension 0, a dispatch that launches nothing)
        rayDesc.Store3(c * P[3].y + P[2].y, n > 0 ? uint3(p.probeResolution * p.probeResolution, n, 1) : uint3(0, 0, 0));  // Width, Height, Depth
        filterArgs.Store3(32 * c, n > 0 ? uint3((p.probeResolution + 7) / 8, (p.probeResolution + 7) / 8, n) : uint3(0, 0, 0));
        filterArgs.Store3(32 * c + 16, n > 0 ? uint3((p.finalResolution + 7) / 8, (p.finalResolution + 7) / 8, n) : uint3(0, 0, 0));
    }
}
#elif MODE == 9
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer marks = ResourceDescriptorHeap[P[1].z];
    if (P[2].x != 0)
    {
        if (id.x == 0) marks.Store(0, 0);
        return;
    }
    if (id.x >= min(marks.Load(0), LRC_HIT_MARKS)) return;
    const LrcParams p = lrcParams(P[0].x);
    const float3 position = asfloat(marks.Load3(16 + id.x * 16));
    if (!all(abs(position) < 1e9)) return;
    const uint own = lrcClipmap(p, position, 0.5);
    if (own >= p.clipmaps) return;
    const uint clipmap = min(own + P[1].w, p.clipmaps - 1);
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
    const int3 corner = int3(floor(lrcCoordFloat(p, position, clipmap) - 0.5));
    for (uint i = 0; i < 8; ++i)
    {
        const int3 c = corner + int3(i & 1, (i >> 1) & 1, i >> 2);
        if (any(c < 0) || any(c >= int(p.grid))) continue;
        indirection[uint3(c.x + int(clipmap * p.grid), c.y, c.z)] = LRC_USED;
    }
}
#elif MODE == 7
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].z];
    if (id.x < p.maxProbes) slots.Store4(id.x * 16, uint4(0, 0, 0, 0));
    if (id.x == 0)
    {
        RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].w];
        for (uint i = 0; i < 32 + 4 * LRC_HISTOGRAM; i += 4) state.Store(i, 0);
    }
}
#else
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    if (id.x >= p.grid * p.clipmaps || id.y >= p.grid || id.z >= p.grid) return;
    RWTexture3D<uint> indirection = ResourceDescriptorHeap[P[0].y];
    const uint e = indirection[id];
    if (e >= LRC_USED) return;
    RWByteAddressBuffer slots = ResourceDescriptorHeap[P[0].z];
    if (slots.Load(e * 16 + 8) == LRC_NEVER_TRACED) indirection[id] = LRC_INVALID;
}
#endif
