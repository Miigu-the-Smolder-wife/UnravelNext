// unx-kernel: cs_6_6 main
// Page requests of the air for the local lights (FroxelIntegrate's shadowed air in-scattering): for every froxel and
// every light of its list with a shadow slot (entry bit 15), every target-mip page the integration's walk crosses on
// the tile-centre segment (VsmLocalAirWalk.hlsli, the same cursor at page cells), where the segment's depth there lies
// between the faces' near and far planes. One group per tile, one thread per depth slice. Runs after the froxel lists
// (shadowPages).
// P[0].x requests UAV (raw), P[0].y froxel lists SRV (raw), P[0].z local lights SRV, P[0].w slot of light SRV
// P[1].x shadow texels per tile (float bits), P[1].y VSM stats UAV (raw; error word). Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"
#include "Passes/Shadow/VsmLocalAirWalk.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint s : SV_GroupIndex)
{
    const FroxelGrid g = froxelGrid(P[0].y);
    if (s >= g.slices) return;
    const uint2 tile = gid.xy;
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint h = lists.Load(g.headerBase + froxelIndex(g, tile, s) * 4);
    const uint first = h >> 6, count = h & 63u;
    if (count == 0) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<VsmLocalLight> locals = ResourceDescriptorHeap[P[0].z];
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[P[0].w];
    const float3 ray = froxelTileRay(g, tile);
    const float toRay = length(ray);
    const float3 dir = ray / toRay;
    const float z0 = froxelNodeDepth(g, s), z1 = froxelNodeDepth(g, s + 1);
    const float3 o = g_cameraPosition + dir * (z0 * toRay);
    const float len = (z1 - z0) * toRay;
    const float width = froxelTileWidth(g, 0.5 * (z0 + z1)) / asfloat(P[1].x);
    bool capped = false;
    [loop] for (uint i = 0; i < count; ++i)
    {
        const uint w = lists.Load(g.indexBase + ((first + i) >> 1) * 4);
        const uint entry = ((first + i) & 1) ? w >> 16 : w & 0xFFFFu;
        if ((entry & 0x8000u) == 0) continue;
        const uint slot = slotOf[entry & 0x7FFFu];
        if (slot == VSM_LOCAL_NONE) continue;
        const VsmLocalLight l = locals[slot];
        uint steps = 0, stuck = 0;
        VsmLocalAirCursor c = vsmLocalAirBegin(o - l.position, dir, len, width);
        float t = 0;
        [loop] while (t < len && steps < VSM_LOCAL_AIR_PAGE_STEPS)
        {
            ++steps;
            uint event;
            const float tE = vsmLocalAirExit(c, VSM_PAGE, t, len, event);
            if (tE <= t && ++stuck > 2)  // grazing a face edge: the integration's walk moves on the same way
            {
                t = min(t + 1e-6 * len, len);
                vsmLocalAirSetFace(c, vsmCubeFace(c.d0 + c.D * t));
                vsmLocalAirLocate(c, t);
                stuck = 0;
                continue;
            }
            if (tE > t) stuck = 0;
            const float za = c.z0 + c.dz * t, zb = c.z0 + c.dz * tE;
            if (max(za, zb) > l.nearM && min(za, zb) < l.farM)
                requests.Store(vsmLocalSlot(slot, c.face, c.m, uint2(c.T) >> VSM_PAGE_SHIFT) * 4, VSM_REQ_PIXEL);
            t = tE;
            if (t < len) vsmLocalAirAdvance(c, VSM_PAGE, tE, event);
        }
        capped = capped || t < len;
    }
    if (WaveActiveAnyTrue(capped) && WaveIsFirstLane())
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].y];
        stats.InterlockedOr(VSM_STATS_ERROR_BYTE, VSM_ERR_MARK_LOCAL_AIR_WALK);
    }
}
