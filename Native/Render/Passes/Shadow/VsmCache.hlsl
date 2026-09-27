// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5
// Sun page cache (shadow.vsm.cache; user decision 2026-09-28: static pages are reused, and the worst case - a sun moving
// without limit, every page redrawn - stays the one path's cost). A sun page's content is the height field of the
// casters over its light-space square: it changes only where a caster moved, deformed, appeared or vanished, or when
// the sun, the caster height range (the raster's depth mapping), the scene or the atlas changed (VsmSystem.cpp: the
// frame is then not "cacheable" and every requested page is drawn, as before). So a page requested this frame keeps
// its physical page and content when it was resident last frame for the same absolute page (tag), and no changed caster
// touched it; only the other requested pages get a free physical page, a clear and the raster. Local-light pages are
// drawn every frame (their lights move with trains and doors; their faces are not cached here).
//   MODE 5 (1 group): clear the used-page bitmap and the changed-caster list header.
//   MODE 0: per scene instance, its caster state (transform and deformation revisions, cast / hidden flags) against last
//           frame's; a caster that changed, appeared or vanished, or one animated every frame (wind, morphs, terrain
//           patches), appends its bounding sphere now and last frame (motion breaks: the break centre; wind and morph
//           bounds added) to the list. Skinned instances: MODE 1.
//   MODE 1 (before MODE 0): per skinned slot of V's bounds (FrameResources::skinBounds: current and previous world
//           spheres), both spheres of a skinned instance casting now or last frame, every frame; an unbounded one (radius
//           < 0) makes the frame uncacheable.
//   MODE 2: per level and changed caster sphere, the resident sun pages of the level under it become stale.
//   MODE 3: per requested sun slot of a cacheable frame, keep: resident last frame, same tag, not stale -> the request is
//           marked kept and its physical page used.
//   MODE 4 (1 group): the free physical pages in page order (deterministic): the pages the scan assigns (VsmScan). With
//           nothing kept (an uncacheable frame) it is 0, 1, 2, .. and the assignment is the one path's.
// Error handling: the changed-caster list has a fixed capacity; past it the frame is uncacheable (flag), never partial.
// P[0] = { caster state UAV (uint4 per instance), changed list UAV (raw: count, flags, 0, 0, then float4 spheres), instance
//          count, VSM constants CBV }
// P[1] = { list capacity (spheres), page table UAV (raw), requests UAV (raw), used-page bitmap UAV (raw) }
// P[2] = { free list UAV (raw: count, 0, 0, 0, then pages), atlas pages, scanned slots, cacheable (1) }
// P[3] = { skin bounds SRV, skin instances SRV, skin count, stats UAV (raw; word 6: kept pages) }
// Frame constants of the main view (scene buffers, wind).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

#define VSM_REQ_KEPT (1u << 31)
#define VSM_CACHE_UNCACHEABLE 1u

void appendSphere(RWByteAddressBuffer list, float3 centre, float radius)
{
    uint at;
    list.InterlockedAdd(0, 1u, at);
    if (at < P[1].x) list.Store4(16 + at * 16, uint4(asuint(centre), asuint(radius)));
    else list.InterlockedOr(4, VSM_CACHE_UNCACHEABLE);
}

// A bound of the largest stretch of the 3 x 3 part (Frobenius norm >= spectral norm).
float stretch(float4 rows[3]) { return sqrt(dot(rows[0].xyz, rows[0].xyz) + dot(rows[1].xyz, rows[1].xyz) + dot(rows[2].xyz, rows[2].xyz)); }

#if MODE == 5
[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
    const uint words = (P[2].y + 31) / 32;
    [loop] for (uint w = i; w < words; w += 256) used.Store(w * 4, 0);
    if (i == 0)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
        list.Store4(0, uint4(0, 0, 0, 0));
    }
}
#elif MODE == 0
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    RWStructuredBuffer<uint4> state = ResourceDescriptorHeap[P[0].x];
    const GpuInstance inst = loadInstance(i);
    const uint castFlags = inst.flags & (INSTANCE_CAST_SHADOW | INSTANCE_HIDDEN);
    const uint4 now = uint4(inst.transformRevision, inst.deformRevision, castFlags, 1);
    const uint4 last = state[i];
    state[i] = now;
    if (inst.bonePalette != UNX_NONE) return;  // skinned: MODE 1 (V's posed bounds)
    const bool casts = castFlags == INSTANCE_CAST_SHADOW, casted = last.w != 0 && last.z == INSTANCE_CAST_SHADOW;
    if (!casts && !casted) return;
    const bool changed = any(last.xyz != now.xyz) || last.w == 0;
    const bool animated = (inst.flags & INSTANCE_WIND) != 0 || inst.morph != UNX_NONE || inst.patch != UNX_NONE;
    if (!changed && !animated) return;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float grow = windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w) + (inst.morph != UNX_NONE ? inst.morphRadius : 0.0);
    const float r = mesh.boundsSphere.w + grow;
    if (casts) appendSphere(list, transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), r * stretch(inst.objectToWorld));
    if (casted || casts)
    {
        const bool broken = (inst.flags & INSTANCE_MOTION_BREAK) != 0;
        const float3 before = broken ? inst.breakCentre : transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz);
        appendSphere(list, before, r * max(stretch(inst.prevObjectToWorld), stretch(inst.objectToWorld)));
    }
}
#elif MODE == 1
[numthreads(64, 1, 1)]
void main(uint j : SV_DispatchThreadID)
{
    if (j >= P[3].z) return;
    StructuredBuffer<float4> bounds = ResourceDescriptorHeap[P[3].x];
    StructuredBuffer<uint> instances = ResourceDescriptorHeap[P[3].y];
    const GpuInstance inst = loadInstance(instances[j]);
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    // casting now or last frame (MODE 1 runs before MODE 0 rewrites the state: it holds last frame's flags)
    RWStructuredBuffer<uint4> state = ResourceDescriptorHeap[P[0].x];
    const uint4 last = state[instances[j]];
    const bool casts = (inst.flags & (INSTANCE_CAST_SHADOW | INSTANCE_HIDDEN)) == INSTANCE_CAST_SHADOW, casted = last.w != 0 && last.z == INSTANCE_CAST_SHADOW;
    if (!casts && !casted) return;
    const float4 now = bounds[2 * j], before = bounds[2 * j + 1];
    if (now.w < 0 || before.w < 0)
    {
        list.InterlockedOr(4, VSM_CACHE_UNCACHEABLE);
        return;
    }
    appendSphere(list, now.xyz, now.w);
    appendSphere(list, before.xyz, before.w);
}
#elif MODE == 2
// The resident sun pages of level k under a world sphere become stale (the sphere's light-space square, clamped to the
// level's window; at most VSM_TABLE^2 pages, 64 lanes).
void staleUnder(ConstantBuffer<VsmConstants> c, RWByteAddressBuffer table, float3 centre, float radius, uint k, uint lane)
{
    const float3 ls = vsmLightSpaceAt(c, centre, k);
    const float pageSize = vsmPageSize(k);
    const int2 lo = max(int2(floor((ls.xy - radius) / pageSize)), vsmOrigin(c, k));
    const int2 hi = min(int2(floor((ls.xy + radius) / pageSize)), vsmOrigin(c, k) + (int)VSM_TABLE - 1);
    if (any(lo > hi)) return;
    const uint2 size = uint2(hi - lo + 1);
    [loop] for (uint i = lane; i < size.x * size.y; i += 64)
    {
        const int2 page = lo + int2(i % size.x, i / size.x);
        const uint slot = vsmSlot(page, k);
        const uint2 e = table.Load2(slot * 8);
        if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == vsmTag(page) && (e.x & VSM_FLAG_STALE) == 0) table.InterlockedOr(slot * 8, VSM_FLAG_STALE);
    }
}
#define STALE_GROUPS_PER_LEVEL 32u
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint2 header = list.Load2(0);
    if (P[2].w == 0 || header.y != 0) return;  // uncacheable: nothing is kept anyway
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[1].y];
    const uint count = min(header.x, P[1].x), k = group.x;
    [loop] for (uint i = group.y; i < count; i += STALE_GROUPS_PER_LEVEL)
    {
        const float4 s = asfloat(list.Load4(16 + i * 16));
        staleUnder(c, table, s.xyz, s.w, k, lane);
    }
}
#elif MODE == 3
[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= min(P[2].z, VSM_SUN_SLOTS) || P[2].w == 0) return;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    if (list.Load(4) != 0) return;  // uncacheable
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[1].z];
    const uint req = requests.Load(slot * 4);
    bool kept = false;
    if (req != 0)
    {
        ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
        RWByteAddressBuffer table = ResourceDescriptorHeap[P[1].y];
        const uint2 e = table.Load2(slot * 8);
        const uint k = slot / VSM_SLOTS_PER_LEVEL, phys = e.x & VSM_PHYS_MASK;
        const uint tag = vsmTag(vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k));
        kept = (e.x & VSM_FLAG_RESIDENT) != 0 && (e.x & VSM_FLAG_STALE) == 0 && e.y == tag && phys < P[2].y;
        if (kept)
        {
            requests.Store(slot * 4, req | VSM_REQ_KEPT);
            RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
            used.InterlockedOr((phys >> 5) * 4, 1u << (phys & 31u));
        }
    }
    const uint n = WaveActiveCountBits(kept);
    if (WaveIsFirstLane() && n)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[3].w];
        stats.InterlockedAdd(24, n);
    }
}
#elif MODE == 4
#define FREE_THREADS 1024u
groupshared uint g_free[FREE_THREADS];
[numthreads(FREE_THREADS, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
    RWByteAddressBuffer free = ResourceDescriptorHeap[P[2].x];
    const uint pages = P[2].y, per = (pages + FREE_THREADS - 1) / FREE_THREADS, first = lane * per;  // per <= 16 (16,384 pages)
    uint count = 0;
    [loop] for (uint i = 0; i < per; ++i)
    {
        const uint p = first + i;
        if (p < pages && ((used.Load((p >> 5) * 4) >> (p & 31u)) & 1u) == 0) ++count;
    }
    g_free[lane] = count;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (uint d = 1; d < FREE_THREADS; d <<= 1)
    {
        const uint v = lane >= d ? g_free[lane - d] : 0;
        GroupMemoryBarrierWithGroupSync();
        g_free[lane] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint at = g_free[lane] - count;
    [loop] for (uint j = 0; j < per; ++j)
    {
        const uint p = first + j;
        if (p < pages && ((used.Load((p >> 5) * 4) >> (p & 31u)) & 1u) == 0) free.Store(16 + (at++) * 4, p);
    }
    if (lane == FREE_THREADS - 1) free.Store(0, g_free[lane]);
}
#endif
