// unx-kernel: cs_6_6 main
// Caster dirty rules (ARCHITECTURE 2.3): a resident page becomes stale when a shadow caster overlapping it
//  (a) changed its transform or deformation (skin) since the last frame: pages under both the old and the new bounds;
//  (b) moved by wind more than windTexels texels of that level since the page was rendered (on distant pages the sway
//      is sub-texel and the page stays cached).
// The wind displacement is evaluated with the shared windOffset (Deformation.hlsli) at the caster's highest object-space
// point, where the v1 model's displacement (height squared, one direction) is largest; see S_STATUS_KO.md.
// P[0].x page table UAV (raw), P[0].y page metadata SRV (uint4), P[0].z last revisions UAV (uint2 per instance),
// P[0].w VSM constants SRV, P[1].x constants offset. Frame constants of the main view (scene buffers, wind, time).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

void markRange(VsmConstants c, RWByteAddressBuffer table, StructuredBuffer<uint4> meta, GpuInstance inst, float3 centre, float radius,
               bool moved, float3 objTop, float scale)
{
    const float3 ls = vsmLightSpace(c, centre);
    [loop] for (uint k = 0; k < VSM_LEVELS; ++k)
    {
        const float pageSize = c.level[k].texel * VSM_PAGE;
        const int2 lo = max(int2(floor((ls.xy - radius) / pageSize)), c.level[k].origin);
        const int2 hi = min(int2(floor((ls.xy + radius) / pageSize)), c.level[k].origin + (int)VSM_TABLE - 1);
        if (any(lo > hi)) continue;
        [loop] for (int y = lo.y; y <= hi.y; ++y)
            [loop] for (int x = lo.x; x <= hi.x; ++x)
            {
                const int2 page = int2(x, y);
                const uint slot = vsmSlot(page, k);
                const uint2 e = table.Load2(slot * 8);
                if ((e.x & VSM_FLAG_RESIDENT) == 0 || e.y != vsmTag(page) || (e.x & VSM_FLAG_STALE) != 0) continue;
                bool stale = moved;
                if (!stale)
                {
                    const float renderTime = asfloat(meta[e.x & VSM_PHYS_MASK].z);
                    const float3 d = windOffset(inst, objTop, c.time) - windOffset(inst, objTop, renderTime);
                    stale = length(d) * scale > c.windTexels * c.level[k].texel;
                }
                if (stale) table.InterlockedOr(slot * 8, VSM_FLAG_STALE);
            }
    }
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const VsmConstants c = vsmLoadConstants(P[0].w, P[1].x);
    if (i >= c.instanceCount) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint4> meta = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint2> lastRevision = ResourceDescriptorHeap[P[0].z];
    const GpuInstance inst = loadInstance(i);
    const uint2 rev = uint2(inst.transformRevision, inst.deformRevision);
    const bool moved = any(lastRevision[i] != rev);
    lastRevision[i] = rev;
    if ((inst.flags & INSTANCE_CAST_SHADOW) == 0) return;
    const bool windy = (inst.flags & INSTANCE_WIND) != 0 && inst.windStiffness > 0 && g_windSpeed > 0;
    if (!moved && !windy) return;
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float scale = length(inst.objectToWorld[0].xyz);
    // Deformed bounds: the bind-pose sphere grown by the largest wind displacement (skinned bounds: S_STATUS_KO.md).
    const float radius = (mesh.boundsSphere.w + windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w)) * scale;
    const float3 objTop = float3(0, mesh.boundsMax.y, 0);
    markRange(c, table, meta, inst, transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), radius, moved, objTop, scale);
    if (moved) markRange(c, table, meta, inst, transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz), radius, true, objTop, scale);
}
