// unx-kernel: cs_6_6 main
// Redesign V2 P1 (RENDERER_REDESIGN_V2 1.1b, gi.parent_prior): the parent work of this frame's updates, after the selection
// and before any ray or integration touches an entry, so every parent is read in its state before this frame's updates
// (GiIntegrate reading parents while other groups updated them made debug.deterministic runs differ).
// One group per update slot (the selected entries, then background: GiIntegrate's slots). Lane 0 finds the parent - the
// cell containing the entry one level up, else two, same normal class (giParentKey):
//   while the entry has fewer than 16 measured updates since its creation or restart, its level + 1 parent is touched and
//   requested (a hit-list request: the next frame's selection sees it), and created when missing (default mode only: the
//   deterministic mode admits entries through its sorted admission, whose producers are bounded);
//   a parent with 16 measured updates is converged: it seeds a never-updated entry (history of this epoch empty and no
//   update ever: GiInvalidate's restarts keep what they have, their parents may hold the same stale light) - the map at the
//   entry's 81 directions (the parent's map evaluated there), its texels and emitter texels toward each texel's centre
//   direction (nearest), its SH (world frame, sun visibility half included) - and marks it (GI_SH_RESTART bit 16:
//   GiIntegrate weighs the seed as kappa updates);
//   and for a converged entry with a converged parent: one sample of the parent-child relative difference of the anchor
//   irradiance (GiBegin's running delta^2, the seed's weight).
// P[0] = { cache UAV, updates per frame, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

groupshared uint gs_parent;  // the converged parent, GI_ENTRY_PENDING: none
groupshared uint gs_seed;    // 1: this entry is seeded from it

[numthreads(128, 1, 1)]
void main(uint lane : SV_GroupIndex, uint slot : SV_GroupID)
{
    if (slot >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    uint entry;
    bool background;
    if (!giUpdateSlot(b, h, slot, entry, background)) return;  // uniform over the group
    const uint restartCount = giRestartUpdates(b, h, entry);
    const float3 na = giAnchorNormal(b, h, entry);
    if (lane == 0)
    {
        uint parent = GI_ENTRY_PENDING;
        const uint2 keyWords = b.Load2(h.offMeta + entry * 16);
        const uint64_t key = (uint64_t)keyWords.x | ((uint64_t)keyWords.y << 32);
        [loop] for (uint steps = 1; steps <= 2 && parent == GI_ENTRY_PENDING; ++steps)
        {
            if ((uint)(key & 31u) + steps > h.maxLevel) break;
            const uint64_t pk = giParentKey(key, steps);
            uint found = giFind(b, h, pk);
            if (found == GI_ENTRY_PENDING && steps == 1 && restartCount < 16 && (h.flags & 1u) == 0)
            {
                bool created;
                found = giFindOrCreate(b, h, pk, asfloat(b.Load3(h.offAnchor + entry * 16)), na, created);
            }
            if (found == GI_ENTRY_PENDING) continue;
            if (steps == 1 && restartCount < 16)
            {
                giTouch(b, h, found);
                giRequestHit(b, h, found);
            }
            if (giRestartUpdates(b, h, found) >= 16) parent = found;
        }
        const uint a = h.offSh + entry * GI_SH_STRIDE;
        gs_parent = parent;
        gs_seed = parent != GI_ENTRY_PENDING && giHistory(b, h, entry) == 0 && b.Load(a + GI_SH_UPDATES) == 0 ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint parent = gs_parent;
    if (parent == GI_ENTRY_PENDING) return;  // uniform
    float3 t, bt, pt, pbt;
    giBasis(na, t, bt);
    const float3 np = giAnchorNormal(b, h, parent);
    giBasis(np, pt, pbt);
    if (gs_seed != 0)
    {
        if (lane < GI_TEXEL_COUNT)
        {
            // the parent's texel (and emitter texel) toward this texel's centre direction, nearest
            const float3 l = giHemiOctDecode((float2(lane % GI_TEXELS, lane / GI_TEXELS) + 0.5) / GI_TEXELS);
            const float3 w = t * l.x + bt * l.y + na * l.z;
            float3 lp = float3(dot(w, pt), dot(w, pbt), max(dot(w, np), 0.0));
            lp = dot(lp, lp) > 1e-12 ? normalize(lp) : float3(0, 0, 1);
            const uint2 tp = min(uint2(giHemiOctEncode(lp) * GI_TEXELS), uint2(GI_TEXELS - 1, GI_TEXELS - 1));
            const uint pi = tp.y * GI_TEXELS + tp.x;
            b.Store2(h.offTexels + (entry * GI_TEXEL_COUNT + lane) * 8, b.Load2(h.offTexels + (parent * GI_TEXEL_COUNT + pi) * 8));
            b.Store(giEmitterOffset(h) + (entry * GI_TEXEL_COUNT + lane) * 4, b.Load(giEmitterOffset(h) + (parent * GI_TEXEL_COUNT + pi) * 4));
        }
        if (lane < GI_IRR_N * GI_IRR_N)
        {
            const float3 nj = giHemiOctDecode((float2(lane % GI_IRR_N, lane / GI_IRR_N) + 0.5) / (float)GI_IRR_N);
            const float3 e = giIrrMapAt(b, h, parent, np, t * nj.x + bt * nj.y + na * nj.z);
            b.Store(h.offIrr + entry * GI_IRR_STRIDE + lane * 4, giPackRgb9e5(e * GI_STORE_SCALE));
        }
        if (lane == 0)
        {
            const uint pa = h.offSh + parent * GI_SH_STRIDE, ea = h.offSh + entry * GI_SH_STRIDE;
            b.Store4(ea, b.Load4(pa));
            b.Store4(ea + 16, b.Load4(pa + 16));
            b.Store4(ea + 32, b.Load4(pa + 32));
            b.Store2(ea + 48, b.Load2(pa + 48));
            b.Store(ea + GI_SH_RESTART, 1u << 16);  // seeded (GiIntegrate)
            b.InterlockedAdd(GI_P1_STAT_PRIORS, 1u);
        }
    }
    else if (lane == 0 && restartCount >= 16)
    {
        // a sample of the parent-child difference of converged cells (the anchor irradiance, luminance)
        const float3 lw = float3(0.2126, 0.7152, 0.0722);
        const float c = dot(giIrrUnpack(b.Load(h.offIrr + entry * GI_IRR_STRIDE + GI_IRR_POLE * 4)) * GI_LOAD_SCALE, lw);
        const float q = dot(giIrrMapAt(b, h, parent, np, na), lw);
        const float d = (c - q) / max(max(c, q), 1e-6);
        b.InterlockedAdd(GI_P1_DELTA_SUM, (uint)(min(d * d, 1.0) * 65536.0));
        b.InterlockedAdd(GI_P1_DELTA_COUNT, 1u);
    }
}
