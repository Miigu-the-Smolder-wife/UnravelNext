// unx-kernel: cs_6_6 main
// Per page slot, before allocation: clears last frame's dirty flag; marks every resident page stale on a scene-wide
// invalidation (sun direction, scene reload); applies the wind dirty rule (ARCHITECTURE 2.3 (b)) per page: the largest
// wind displacement bound of the casters drawn into the page times the wind change factor since the page's render
// exceeds windTexels texels of its level, or the wind itself changed; releases pages that left their level's window
// (clipmap scroll: the slot now maps a different absolute page) or were not requested for cacheFrames frames.
// P[0].x page table UAV (raw), P[0].y requests SRV (raw), P[0].z page metadata UAV, P[0].w free list UAV (raw)
// Local-light slots: released when their light changed (generation) or left, aged as above; the wind rule compares with
// the texel at the page's nearest caster depth (2 z / res).
// P[1].x VSM constants CBV, P[1].y local lights SRV (StructuredBuffer<VsmLocalLight>)
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_TOTAL_SLOTS) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    uint2 e = table.Load2(slot * 8);
    if ((e.x & VSM_FLAG_RESIDENT) == 0) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].x];
    ByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[0].z];
    const uint phys = e.x & VSM_PHYS_MASK;
    const bool requested = requests.Load(slot * 4) != 0;
    const VsmPageMeta m = meta[phys];
    bool scrolled;
    float texel;
    if (slot < VSM_SUN_SLOTS)
    {
        const uint k = slot / VSM_SLOTS_PER_LEVEL;
        scrolled = e.y != vsmTag(vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k));
        texel = vsmTexel(k);
    }
    else
    {
        StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[1].y];
        uint light, face, mip;
        uint2 page;
        vsmLocalSlotParts(slot, light, face, mip, page);
        const VsmLocalLight l = lights[light];
        scrolled = l.active == 0 || e.y != l.generation;  // the slot's light changed, moved or left
        const float nearest = m.maxHeight == VSM_EMPTY ? l.farM : -vsmDecode(m.maxHeight);
        texel = 2 * max(nearest, l.nearM) / vsmLocalRes(mip);
    }
    const bool aged = !requested && (c.frame - m.lastRequested) > c.cacheFrames;
    if (scrolled || aged)
    {
        table.Store2(slot * 8, uint2(0, 0));
        meta[phys] = (VsmPageMeta)0;
        RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[0].w];
        uint at;
        freeList.InterlockedAdd(0, 1, at);
        freeList.Store(4 + at * 4, phys);
        return;
    }
    e.x &= ~VSM_FLAG_DIRTY;
    if (c.sceneInvalidate) e.x |= VSM_FLAG_STALE;
    if (m.windCaster != 0 &&
        (c.windChanged != 0 || asfloat(m.windAmplitude) * windChangeFactor(asfloat(m.renderTime), c.time) > c.windTexels * texel))
        e.x |= VSM_FLAG_STALE;
    table.Store(slot * 8, e.x);
}
