// Public shadow lookups (INTERFACES_KO.md 5.6, 7.3). Owner: S. Consumers: M's shading kernels, R's ray hits.
//   shadowVisibility R32_UINT: 4 slots x 8 bit unorm (0 = full shadow, 255 = fully lit). Slot 0 = sun, slots 1-3 = the
//   first three shadow-casting local lights of the pixel's froxel light list, in list order.
#ifndef UNX_SHADOW_VISIBILITY_HLSLI
#define UNX_SHADOW_VISIBILITY_HLSLI
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"
#include "Passes/Shadow/VsmLocalSample.hlsli"

float shadowSlot(uint packed, uint slot) { return ((packed >> (8 * slot)) & 0xFFu) / 255.0; }

// This frame's virtual shadow maps (Docs/Design/Requests/20260925_S_sun_visibility_at.md): bindless indices of the page
// table, the physical pool (raw buffers), the pages' block hierarchy, the search bound grid, the VSM constants CBV, and for
// the local lights their shadow-slot records (lights: FrameResources::vsmLocalLights) and the scene light -> shadow slot
// map (pad0: FrameResources::vsmSlotOfLight; Docs/Design/Requests/20260925_S_local_shadow_lookups.md).
struct ShadowSrvs
{
    uint pageTable, pool, blocks, searchBound;
    uint constants, lights, pad0, pad1;
};

// Visibility slot (1-3) of scene light lightIndex for the main-view pixel at view depth linearDepth: its ordinal among the
// shadow-casting lights of the pixel's froxel list (INTERFACES 7.3); 0xFFFFFFFF when it is not among the first three
// (shadowVisibilityDirect then) or not in the list.
uint shadowSlotOfLight(FroxelSrvs f, uint2 pixel, float linearDepth, uint lightIndex)
{
    const uint2 range = froxelLightRange(f, pixel, linearDepth);
    uint ordinal = 0;
    [loop] for (uint i = 0; i < range.y && ordinal < 3; ++i)
    {
        const uint li = froxelLight(f, range.x + i);
        if (!lightCastsShadow(loadLight(li))) continue;
        ++ordinal;
        if (li == lightIndex) return ordinal;
    }
    return 0xFFFFFFFFu;
}

// Visibility in [0, 1] of scene light lightIndex at a world point with geometric normal: the local-light estimator of the
// visibility slots (VsmLocalSample.hlsli) on the finest resident mip there. For shadow-casting lights past the third of a
// pixel's list; 1 for lights without a shadow slot.
float shadowVisibilityDirect(ShadowSrvs s, uint lightIndex, float3 worldPos, float3 normal)
{
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[s.pad0];
    const uint slot = slotOf[lightIndex];
    if (slot == VSM_LOCAL_NONE) return 1;
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[s.lights];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    VsmLocalResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[s.pool];
    r.blocks = ResourceDescriptorHeap[s.blocks];
    return vsmLocalVisibility(r, lights[slot], slot, worldPos, normal, 0.0, c.receiverBiasTexels, c.maxReceiverSlope, c.searchTaps, c.filterTaps);
}

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
