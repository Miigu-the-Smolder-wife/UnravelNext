// Public shadow lookups (INTERFACES_KO.md 5.6, 7.3). Owner: S. Consumers: M's shading kernels, R's ray hits.
//   shadowVisibility R32_UINT: 4 slots x 8 bit unorm (0 = full shadow, 255 = fully lit). Slot 0 = sun, slots 1-3 = the
//   first three shadow-casting local lights of the pixel's froxel light list, in list order.
#ifndef UNX_SHADOW_VISIBILITY_HLSLI
#define UNX_SHADOW_VISIBILITY_HLSLI
#include "Passes/Shadow/VsmSample.hlsli"

float shadowSlot(uint packed, uint slot) { return ((packed >> (8 * slot)) & 0xFFu) / 255.0; }

// This frame's virtual shadow maps (Docs/Design/Requests/20260925_S_sun_visibility_at.md): bindless indices of the page
// table, the physical pool (raw buffers), the pages' block hierarchy, the search bound grid, and the VSM constants CBV.
struct ShadowSrvs
{
    uint pageTable, pool, blocks, searchBound;
    uint constants, lights, pad0, pad1;
};

// Sun visibility in [0, 1] at a world point with geometric normal (ray hits, R): the direct view's estimator (SMRT:
// reach classification, blocker search, disk filter; vsmSunVisibility) on the level whose texel matches 'footprint'
// (metres, the ray cone's width at the hit), or on one of the three finer levels when that page is not resident (finer
// texels: at least as accurate). resident = false when none holds the point (off-screen hits): the caller traces a
// shadow ray there. The same estimator as the direct view and the planar reflection camera: shadows agree across them.
float shadowSunVisibilityAt(ShadowSrvs s, float3 worldPos, float3 normal, float footprint, out bool resident)
{
    VsmResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[s.pool];
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    const float2 uv = vsmLightSpace(c, worldPos).xy;
    const uint k = vsmLevelForFootprint(c, footprint);
    uint level = 0xFFFFFFFFu;
    [loop] for (uint j = 0; j < 4 && j <= k; ++j)
        if (vsmEntry(r, vsmAbsPage(vsmAbsTexel(c, uv, k - j)), k - j) != 0)
        {
            level = k - j;
            break;
        }
    resident = level != 0xFFFFFFFFu;
    if (!resident) return 1;
    uint path;
    return vsmSunVisibility(r, worldPos, normal, vsmTexel(level), c.tanSunRadius, c.searchTaps, c.filterTaps, path);
}

#endif
