// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5,6
// Page cache (shadow.vsm.cache; design choice: static pages are reused, and the worst case - a sun moving without
// limit, every page redrawn - stays the one path's cost). A sun page's content is the height field of the casters over
// its light-space square: it changes only where a caster moved, deformed, appeared or vanished, or when the sun, the
// caster height range (the raster's depth mapping), the scene or the atlas changed (VsmSystem.cpp: the frame is then not
// "cacheable" and every requested page is drawn, as before). So a page requested this frame keeps its physical page and
// content when it was resident last frame for the same absolute page (tag), and no changed caster touched it; only the
// other requested pages get a free physical page, a clear and the raster. A local light's page is a face depth of the
// casters within its range: its tag is the light's generation (VsmSystem.cpp: position, range, emitter radius, type and
// the shadow slot's light), and a light whose range meets a changed caster's sphere has every page redrawn (MODE 6; a
// light moving with a train or a door changes its generation).
//   MODE 5 (1 group): clear the used-page bitmap and the changed-caster list header.
//   MODE 0: per scene instance, its caster state (transform and deformation revisions, cast / hidden flags) against last
//           frame's; a caster that changed, appeared or vanished, or one animated every frame (wind, morphs, terrain
//           patches), appends its bounding sphere now and last frame (motion breaks: the break centre; wind and morph
//           bounds added) to the list. Skinned instances: MODE 1.
//   MODE 1 (before MODE 0): per skinned slot of V's bounds (FrameResources::skinBounds: current and previous world
//           spheres), both spheres of a skinned instance casting now or last frame, every frame; an unbounded one (radius
//           < 0) makes the frame uncacheable.
//   MODE 2: per level and changed caster sphere, the resident sun pages of the level under it become stale - unless the
//           sphere lies under the page's stored surface (shadow.vsm.cache_hzb_filter, sphereUnderPage below).
//   MODE 6: per local shadow slot, whether its light's range (farM around its position) meets a changed caster's sphere:
//           a bit per light after the used-page bitmap (the light's pages are not kept this frame).
//   MODE 3: per requested slot (sun and local) of a cacheable frame, keep: resident last frame, same tag, not stale (sun)
//           or its light unchanged in range (local) -> the request is marked kept and its physical page used.
//   MODE 4 (1 group): the free physical pages in page order (deterministic): the pages the scan assigns (VsmScan). With
//           nothing kept (an uncacheable frame) it is 0, 1, 2, .. and the assignment is the one path's.
// What a change is worth at a level (shadow.vsm.cache_min_change_texels, e texels; the batch of 2026-10-02: a lake's
// 3,345 wind-moved trees made every resident page of every level stale every frame - 12 levels x 5 ms): each sphere
// carries a code in its radius' 6 low mantissa bits,
//   rigid (63): the caster moved, appeared, vanished, or deforms without a bound (morphs, terrain patches). The level's
//           pages under it become stale when the caster is at least e texels of that level in radius (a smaller one
//           changes less than a texel of the page);
//   wind (0..62, the amplitude B = 2^((code - 40) / 4) m: Deformation.hlsli windOffsetBound): the wind moves a point by
//           at most 0.8 B over any time and by B x windChangeFactor(dt) a frame, so a page drawn N frames ago is off by
//           at most N x that. A level whose e texels exceed 0.8 B never becomes stale from it; the others every
//           N = floor(e texel / (B x windChangeFactor(dt))) frames (1..64), each page on its own frame of the N (a hash
//           of its slot): the shadow of a wind-moved caster is never more than e texels behind its geometry.
// Static / dynamic pages (shadow.vsm.static_separate, P[4].w bit 0; the reference's r.Shadow.Virtual.Cache.StaticSeparate).
// Every caster is in one of two sets: movable (Scene.hlsli INSTANCE_MOVABLE_FLAGS, a bone palette, morph or terrain patch,
// or an instance past the scene's uploaded ones) or not. A sun page has a static copy (the casters that are not movable,
// in the static atlas) and its sampled page = that copy merged with the movable casters' raster. A changed caster that
// is movable now and was when the page was drawn touches only the second part: its sphere carries bit 6 beside the
// change code, MODE 2 marks the pages under it VSM_FLAG_STALE_DYNAMIC, and MODE 3 keeps such a page (its physical page
// and static copy) with VSM_REQ_DYNAMIC: the scan lists it for a clear of the sampled page, the movable casters' raster
// and the merge. A change of a caster that is not movable - or of one that changed set, which stands in the other set's
// content - makes the page stale as before (VSM_FLAG_STALE: everything is drawn anew). The set is part of the caster's
// state word (VSM_CASTER_MOVABLE), so a change of set is a change.
// The local lights' pages have the same two parts (shadow.vsm.local_static_separate, P[4].w bit 1): a light whose range
// meets only changes of the movable casters' part (MODE 6: its bit in a second set of light bits) keeps its pages and
// their static copies, and MODE 3 marks its kept requests VSM_REQ_DYNAMIC - the movable casters of every kept page of
// the light are drawn anew; a change of a caster that is not movable draws the light's pages anew as before.
// Error handling: the changed-caster list has a fixed capacity; past it the frame is uncacheable (flag), never partial.
// P[0] = { caster state UAV (uint4 per instance), changed list UAV (raw: count, flags, 0, 0, then float4 spheres), instance
//          count, VSM constants CBV }
// P[1] = { list capacity (spheres), page table UAV (raw), requests UAV (raw), used-page bitmap UAV (raw) }
// P[2] = { free list UAV (raw: count, 0, 0, 0, then pages), atlas pages, scanned slots, cacheable (1) }
// P[3] = { skin bounds SRV, skin instances SRV, skin count, stats UAV (raw; word 6: kept pages) }
// P[4] = { local lights SRV (VsmLocalLight per shadow slot), local shadow slots in use, page blocks SRV (raw, VsmPageMax:
//          MODE 2's HZB filter; 0xFFFFFFFF: off), bit 0: static_separate, bit 1: local_static_separate }
// P[5] = { asuint(e: the least change that makes a page stale, texels of its level), asuint(windChangeFactor(dt)), frame index,
//          the scene's uploaded instances (GpuScene::staticInstanceCount: the instances from there on are movable) }
// Frame constants of the main view (scene buffers, wind).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

#define VSM_REQ_KEPT (1u << 31)
#define VSM_CACHE_UNCACHEABLE 1u
// The local lights' changed bits: VSM_LOCAL_LIGHTS / 32 words after the used-page bitmap (whose words cover the pages).
uint localChangedOffset() { return (P[2].y + 31) / 32 * 4; }
// ... and after them the bits of the lights met only by changes of the movable casters' part (local_static_separate).
uint localDynamicOffset() { return localChangedOffset() + VSM_LOCAL_LIGHTS / 32 * 4; }

#define VSM_CASTER_MOVABLE (1u << 8)  // caster state word z, beside the cast / hidden flags: the caster's set (static_separate)
#define VSM_CHANGE_RIGID 63u
uint changeCode(float amplitude) { return (uint)clamp(ceil(log2(max(amplitude, 1e-6)) * 4.0 + 40.0), 0.0, 62.0); }  // (rounded up)
float changeAmplitude(uint code) { return exp2((float(code) - 40.0) / 4.0); }
// (the radius is read back with its 7 low bits set: never under the caster's; bit 6: the change is of the movable
// casters' part of a page alone, static_separate)
void appendSphere(RWByteAddressBuffer list, float3 centre, float radius, uint code, bool dynamicOnly)
{
    uint at;
    list.InterlockedAdd(0, 1u, at);
    if (at < P[1].x) list.Store4(16 + at * 16, uint4(asuint(centre), (asuint(radius) & ~127u) | code | (dynamicOnly ? 64u : 0u)));
    else list.InterlockedOr(4, VSM_CACHE_UNCACHEABLE);
}

// A bound of the largest stretch of the 3 x 3 part (Frobenius norm >= spectral norm).
float stretch(float4 rows[3]) { return sqrt(dot(rows[0].xyz, rows[0].xyz) + dot(rows[1].xyz, rows[1].xyz) + dot(rows[2].xyz, rows[2].xyz)); }

#if MODE == 5
[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
    const uint words = (P[2].y + 31) / 32 + 2 * (VSM_LOCAL_LIGHTS / 32);  // (and the local lights' two sets of changed bits)
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
    const uint castMask = INSTANCE_CAST_SHADOW | INSTANCE_HIDDEN, castFlags = inst.flags & castMask;
    // static_separate: the caster's set (V draws the same sets: VisibilityCommon.hlsli instanceInSet)
    const bool separate = (P[4].w & 1u) != 0;
    const bool movable = separate && ((inst.flags & INSTANCE_MOVABLE_FLAGS) != 0 || inst.bonePalette != UNX_NONE || inst.morph != UNX_NONE ||
                                      inst.patch != UNX_NONE || i >= P[5].w);
    const uint4 now = uint4(inst.transformRevision, inst.deformRevision, castFlags | (movable ? VSM_CASTER_MOVABLE : 0u), 1);
    const uint4 last = state[i];
    state[i] = now;
    const bool casts = castFlags == INSTANCE_CAST_SHADOW, casted = last.w != 0 && (last.z & castMask) == INSTANCE_CAST_SHADOW;
    if (!casts && !casted) return;
    // The change is of the movable casters' part of a page alone when the caster is movable and was when the pages were
    // drawn (one seen for the first time stood in no page); a caster that changed set stands in the other set's content.
    const bool wasMovable = last.w == 0 || (last.z & VSM_CASTER_MOVABLE) != 0;
    const bool dynamicOnly = movable && wasMovable;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    if (inst.bonePalette != UNX_NONE)
    {
        // skinned: MODE 1 (V's posed bounds); without them (no V this frame) the frame is not cacheable
        if (P[3].z == 0) list.InterlockedOr(4, VSM_CACHE_UNCACHEABLE);
        if (separate && !wasMovable)
        {
            // (it had no palette when the pages were drawn: it stands in their static copies with its rigid bounds)
            const GpuMesh rigid = loadMesh(inst.mesh);
            appendSphere(list, transformPoint(inst.prevObjectToWorld, rigid.boundsSphere.xyz),
                         rigid.boundsSphere.w * max(stretch(inst.prevObjectToWorld), stretch(inst.objectToWorld)), VSM_CHANGE_RIGID, false);
        }
        return;
    }
    const bool changed = any(last.xyz != now.xyz) || last.w == 0;
    const bool animated = (inst.flags & INSTANCE_WIND) != 0 || inst.morph != UNX_NONE || inst.patch != UNX_NONE;
    if (!changed && !animated) return;
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float wind = windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w);
    const float grow = wind + (inst.morph != UNX_NONE ? inst.morphRadius : 0.0);
    const float r = mesh.boundsSphere.w + grow;
    // the wind alone (its amplitude in the world), or anything else
    const bool windOnly = !changed && inst.morph == UNX_NONE && inst.patch == UNX_NONE;
    if (windOnly && !(wind > 0)) return;  // (no wind this frame: nothing moves)
    const uint code = windOnly ? changeCode(wind * stretch(inst.objectToWorld)) : VSM_CHANGE_RIGID;
    if (casts) appendSphere(list, transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), r * stretch(inst.objectToWorld), code, dynamicOnly);
    if (casted || casts)
    {
        const bool broken = (inst.flags & INSTANCE_MOTION_BREAK) != 0;
        const float3 before = broken ? inst.breakCentre : transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz);
        appendSphere(list, before, r * max(stretch(inst.prevObjectToWorld), stretch(inst.objectToWorld)), code, dynamicOnly);
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
    const uint castMask = INSTANCE_CAST_SHADOW | INSTANCE_HIDDEN;
    const bool casts = (inst.flags & castMask) == INSTANCE_CAST_SHADOW, casted = last.w != 0 && (last.z & castMask) == INSTANCE_CAST_SHADOW;
    if (!casts && !casted) return;
    const float4 now = bounds[2 * j], before = bounds[2 * j + 1];
    if (now.w < 0 || before.w < 0)
    {
        list.InterlockedOr(4, VSM_CACHE_UNCACHEABLE);
        return;
    }
    // (static_separate: a skinned caster is movable; one that was not when the pages were drawn: MODE 0's rigid sphere)
    const bool dynamicOnly = (P[4].w & 1u) != 0 && (last.w == 0 || (last.z & VSM_CASTER_MOVABLE) != 0);
    appendSphere(list, now.xyz, now.w, VSM_CHANGE_RIGID, dynamicOnly);
    appendSphere(list, before.xyz, before.w, VSM_CHANGE_RIGID, dynamicOnly);
}
#elif MODE == 2
// HZB filter (shadow.vsm.cache_hzb_filter; the reference's HZB-filtered invalidation). A changed caster whose sphere lies
// under the page's stored surface over all of its footprint in the page has no texel of its own there: where it stood it
// was drawn behind the casters that are stored, and where it stands now it would be (each of its spheres, before and now,
// is tested on its own; a caster that is itself the stored surface at a texel is not under it there: its top is at or
// above what it stored). The page's content is the same with or without it, so the page stays. The block hierarchy
// (VsmPageMax) is the page's HZB: a block's range.x is the lowest stored height of its texels, VSM_EMPTY when one has no
// caster. The footprint's square is read on the block level where it spans at most 2 x 2 blocks.
bool sphereUnderPage(ByteAddressBuffer blocks, uint phys, int2 page, uint k, float3 ls, float radius)
{
    const float texel = vsmTexel(k);
    const int2 corner = page * (int)VSM_PAGE;
    const int2 t0 = clamp(int2(floor((ls.xy - radius) / texel)) - corner, 0, (int)VSM_PAGE - 1);
    const int2 t1 = clamp(int2(floor((ls.xy + radius) / texel)) - corner, 0, (int)VSM_PAGE - 1);
    const uint size = (uint)max(t1.x - t0.x, t1.y - t0.y) + 1;
    const uint m = size <= 8 ? 0u : min((uint)ceil(log2(size / 8.0)), 4u);
    const int shift = 3 + (int)m, perPage = (int)(16u >> m);
    const int2 b0 = t0 >> shift, b1 = t1 >> shift;
    const uint top = vsmEncode(ls.z + radius);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const int2 b = int2((i & 1) ? b1.x : b0.x, (i & 2) ? b1.y : b0.y);
        const uint lowest = blocks.Load((phys * VSM_BLOCK_ENTRIES + vsmBlockOffset(m) + (uint)(b.y * perPage + b.x)) * VSM_BLOCK_BYTES);
        if (lowest == VSM_EMPTY || lowest <= top) return false;
    }
    return true;
}

// The resident sun pages of level k under a world sphere become stale (the sphere's light-space square, clamped to the
// level's window; at most VSM_TABLE^2 pages, 64 lanes). Returns the pages the HZB filter left as they are.
// flag: VSM_FLAG_STALE, or VSM_FLAG_STALE_DYNAMIC for a change of the movable casters' part alone (static_separate).
uint staleUnder(ConstantBuffer<VsmConstants> c, RWByteAddressBuffer table, float3 centre, float radius, uint code, uint k, uint lane, uint flag)
{
    // what the change is worth at this level (the header)
    const float least = asfloat(P[5].x) * vsmTexel(k);
    uint period = 1;
    if (code == VSM_CHANGE_RIGID)
    {
        if (radius < least) return 0;
    }
    else
    {
        const float amplitude = changeAmplitude(code);
        if (0.8 * amplitude < least) return 0;
        period = (uint)clamp(floor(least / max(amplitude * asfloat(P[5].y), 1e-9)), 1.0, 64.0);
    }
    const float3 ls = vsmLightSpaceAt(c, centre, k);
    const float pageSize = vsmPageSize(k);
    const int2 lo = max(int2(floor((ls.xy - radius) / pageSize)), vsmOrigin(c, k));
    const int2 hi = min(int2(floor((ls.xy + radius) / pageSize)), vsmOrigin(c, k) + (int)VSM_TABLE - 1);
    if (any(lo > hi)) return 0;
    const uint2 size = uint2(hi - lo + 1);
    uint spared = 0;
    [loop] for (uint i = lane; i < size.x * size.y; i += 64)
    {
        const int2 page = lo + int2(i % size.x, i / size.x);
        const uint slot = vsmSlot(page, k);
        if (period > 1 && (P[5].z + ((slot * 2654435761u) >> 16)) % period != 0) continue;
        const uint2 e = table.Load2(slot * 8);
        if ((e.x & VSM_FLAG_RESIDENT) == 0 || e.y != vsmTag(page) || (e.x & (VSM_FLAG_STALE | flag)) != 0) continue;
        if (P[4].z != 0xFFFFFFFFu)
        {
            ByteAddressBuffer blocks = ResourceDescriptorHeap[P[4].z];
            if (sphereUnderPage(blocks, e.x & VSM_PHYS_MASK, page, k, ls, radius))
            {
                ++spared;
                continue;
            }
        }
        table.InterlockedOr(slot * 8, flag);
    }
    return spared;
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
    uint spared = 0;
    [loop] for (uint i = group.y; i < count; i += STALE_GROUPS_PER_LEVEL)
    {
        const uint4 s = list.Load4(16 + i * 16);
        spared += staleUnder(c, table, asfloat(s.xyz), asfloat(s.w | 127u), s.w & 63u, k, lane, (s.w & 64u) != 0 ? VSM_FLAG_STALE_DYNAMIC : VSM_FLAG_STALE);
    }
    if (spared != 0)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[3].w];
        stats.InterlockedAdd(4 * 72, spared);  // (sphere, page) pairs the HZB filter left unchanged
    }
}
#elif MODE == 3
[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= P[2].z || P[2].w == 0) return;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    if (list.Load(4) != 0) return;  // uncacheable
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[1].z];
    const uint req = requests.Load(slot * 4);
    bool kept = false;
    if (req != 0)
    {
        RWByteAddressBuffer table = ResourceDescriptorHeap[P[1].y];
        RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
        const uint2 e = table.Load2(slot * 8);
        const uint phys = e.x & VSM_PHYS_MASK;
        uint tag;
        bool unchanged = true, localDynamic = false;
        if (slot < VSM_SUN_SLOTS)
        {
            ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].w];
            const uint k = slot / VSM_SLOTS_PER_LEVEL;
            tag = vsmTag(vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k));
        }
        else
        {
            // a local light's page: its light's generation, and no changed caster in the light's range (MODE 6)
            // (slots of lights no longer in use - the scan's range keeps last frame's - are not kept)
            const uint light = (slot - VSM_SUN_SLOTS) / VSM_LOCAL_LIGHT_SLOTS;
            tag = 0;
            unchanged = light < P[4].y;
            if (unchanged)
            {
                StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[4].x];
                const VsmLocalLight l = lights[light];
                tag = l.generation;
                unchanged = l.active != 0 && ((used.Load(localChangedOffset() + (light >> 5) * 4) >> (light & 31u)) & 1u) == 0;
                localDynamic = ((used.Load(localDynamicOffset() + (light >> 5) * 4) >> (light & 31u)) & 1u) != 0;
            }
        }
        kept = unchanged && (e.x & VSM_FLAG_RESIDENT) != 0 && (e.x & VSM_FLAG_STALE) == 0 && e.y == tag && phys < P[2].y;
        if (kept)
        {
            // (static_separate: a movable caster changed under it - the page and its static copy are kept, the movable
            // casters are drawn anew)
            requests.Store(slot * 4, req | VSM_REQ_KEPT | (((e.x & VSM_FLAG_STALE_DYNAMIC) != 0 || localDynamic) ? VSM_REQ_DYNAMIC : 0u));
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
#elif MODE == 6
// One thread per local shadow slot in use: its light's range meets a changed caster's sphere (the list's spheres, at most
// its capacity) -> the light's changed bit. Uncacheable frames keep nothing anyway.
[numthreads(64, 1, 1)]
void main(uint light : SV_DispatchThreadID)
{
    if (light >= P[4].y || P[2].w == 0) return;
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint2 header = list.Load2(0);
    if (header.y != 0) return;
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[4].x];
    const VsmLocalLight l = lights[light];
    if (l.active == 0) return;
    const uint count = min(header.x, P[1].x);
    const bool localSeparate = (P[4].w & 2u) != 0;
    bool changed = false, changedDynamic = false;
    [loop] for (uint i = 0; i < count && !changed; ++i)
    {
        const uint4 sw = list.Load4(16 + i * 16);
        const float4 s = asfloat(uint4(sw.xyz, sw.w | 127u));  // (the radius with its code bits set: never under the caster's)
        if (distance(s.xyz, l.position) <= s.w + l.farM)
        {
            // (bit 6: a change of the movable casters' part alone - the light's static copies hold)
            if (localSeparate && (sw.w & 64u) != 0) changedDynamic = true;
            else changed = true;
        }
    }
    RWByteAddressBuffer used = ResourceDescriptorHeap[P[1].w];
    if (changed) used.InterlockedOr(localChangedOffset() + (light >> 5) * 4, 1u << (light & 31u));
    else if (changedDynamic) used.InterlockedOr(localDynamicOffset() + (light >> 5) * 4, 1u << (light & 31u));
}
#endif
