// Public shadow lookups (INTERFACES_KO.md 5.6, 7.3). Owner: S. Consumers: M's shading kernels, R's ray hits.
//   shadowVisibility R32_UINT: 4 slots x 8 bit unorm (0 = full shadow, 255 = fully lit). Slot 0 = sun, slots 1-3 = the
//   first three shadow-casting local lights of the pixel's froxel light list, in list order.
#ifndef UNX_SHADOW_VISIBILITY_HLSLI
#define UNX_SHADOW_VISIBILITY_HLSLI
#include "Scene.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/Shadow/ShadowReceiver.hlsli"
#include "Passes/Shadow/VsmSample.hlsli"
#include "Passes/Shadow/VsmLocalSample.hlsli"
#include "Passes/Shadow/VsmLayer.hlsli"

float shadowSlot(uint packed, uint slot) { return ((packed >> (8 * slot)) & 0xFFu) / 255.0; }

// This frame's virtual shadow maps (Docs/Design/Requests/20260925_S_sun_visibility_at.md): bindless indices of the page
// table, the page atlas (pool: an SRV of FrameResources::vsmAtlas; v1.43 one path, the lookups read the atlas SRV from
// the VSM constants, VsmConstants::atlasSrv, so this word is not read), the pages' block hierarchy, the search bound
// grid, the VSM constants CBV, and for
// the local lights their shadow-slot records (lights: FrameResources::vsmLocalLights) and the scene light -> shadow slot
// map (pad0: FrameResources::vsmSlotOfLight; Docs/Design/Requests/20260925_S_local_shadow_lookups.md), and the thin
// casters' transmittance layer (layers = FrameResources::vsmLayers, INTERFACES 5.6 v1.26; 0xFFFFFFFF or 0 = no layer,
// T = 1).
struct ShadowSrvs
{
    uint pageTable, pool, blocks, searchBound;
    uint constants, lights, pad0, layers;
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
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];  // the atlas (VsmConstants; ShadowSrvs.pool is not read)
    r.blocks = ResourceDescriptorHeap[s.blocks];
    return vsmLocalVisibility(r, lights[slot], slot, worldPos, normal, 0.0, c.receiverBiasTexels, c.maxReceiverSlope, c.searchTaps, c.filterTaps);
}

// Receiver of a pixel of the view whose frame constants are bound: the world position and geometric normal from the depth
// buffer (ShadowReceiver.hlsli) and the pixel's footprint in metres. valid = false for sky and pixels outside the view.
struct ShadowPixelReceiver
{
    float3 world;
    float footprint;
    float3 normal;
    uint valid;
};
ShadowPixelReceiver shadowPixelReceiver(uint2 pixel, uint depthSrv, uint gbufferSrv)
{
    ShadowPixelReceiver rc = (ShadowPixelReceiver)0;
    if (pixel.x >= g_viewWidth || pixel.y >= g_viewHeight) return rc;
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthSrv];
    const float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0) return rc;
    rc.world = shadowReceiver(depthTex, gbufferSrv, pixel, depth, rc.normal);
    rc.footprint = 2 * linearDepth(depth) * g_tanHalfFovY / g_viewHeight;
    rc.valid = 1;
    return rc;
}

// Visibility of scene light lightIndex at a pixel receiver: exactly the computation of the visibility slots 1-3 and of
// the overflow list (INTERFACES 7.3; ShadowVisibility.hlsl, ShadowOverflow.hlsl call it), so a fallback that evaluates
// the lights of an over-capacity tile with it agrees with them (the stored slots are this value rounded to 8 bits:
// round(saturate(v) * 255) / 255). 1 for lights without a shadow slot (the slots store 255 there).
float shadowLocalVisibilityAtReceiver(ShadowSrvs s, uint lightIndex, ShadowPixelReceiver rc)
{
    StructuredBuffer<uint> slotOf = ResourceDescriptorHeap[s.pad0];
    const uint slot = slotOf[lightIndex];
    if (slot == VSM_LOCAL_NONE) return 1;
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[s.lights];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    VsmLocalResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];  // the atlas (VsmConstants; ShadowSrvs.pool is not read)
    r.blocks = ResourceDescriptorHeap[s.blocks];
    return vsmLocalVisibility(r, lights[slot], slot, rc.world, rc.normal, rc.footprint, c.receiverBiasTexels, c.maxReceiverSlope, c.searchTaps,
                              c.filterTaps);
}
float shadowLocalVisibilityAtPixel(ShadowSrvs s, uint lightIndex, uint2 pixel, uint depthSrv, uint gbufferSrv)
{
    const ShadowPixelReceiver rc = shadowPixelReceiver(pixel, depthSrv, gbufferSrv);
    return rc.valid ? shadowLocalVisibilityAtReceiver(s, lightIndex, rc) : 1.0;
}

// Transmittance in [0, 1] of the thin casters above a world point (VSM transmittance layer, VsmLayer.hlsli; INTERFACES
// 5.6 v1.26): level k for 'footprint', the layer's mip for the filter radius 'reach' (m), T(h) at the point's height.
// 1 where the page is not resident or has no thin casters. shadowSunVisibilityAt and the visibility slot 0 multiply it.
float shadowSunTransmittanceAt(ShadowSrvs s, float3 worldPos, float footprint, float reach)
{
    // No layer: 0xFFFFFFFF, or 0 from callers written before v1.26 (descriptor 0 is taken at device creation and is never
    // this frame's layer buffer).
    if (s.layers == 0xFFFFFFFFu || s.layers == 0) return 1;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    VsmResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];  // the atlas (VsmConstants; ShadowSrvs.pool is not read)
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    const uint k = vsmLevelForFootprint(c, footprint);
    const float3 ls = vsmLightSpaceAt(c, worldPos, k);
    const int2 page = vsmAbsPage(vsmAbsTexel(c, ls.xy, k));
    ByteAddressBuffer layers = ResourceDescriptorHeap[s.layers];
    return vsmLayerTransmittance(layers, c.poolPagesX * c.poolPagesY, vsmEntry(r, page, k), page, ls.xy, k, reach, ls.z);
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
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];  // the atlas (VsmConstants; ShadowSrvs.pool is not read)
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    const uint k = vsmLevelForFootprint(c, footprint);
    uint level = 0xFFFFFFFFu;
    // A level serves a hit only where the direct view's residency contract holds there (VsmPropagate: a pixel's page at
    // level L brings the 3 x 3 pages around it on L + 1 .. L + 3, where the blocker search and penumbra taps fall back):
    // the point's page and the 3 x 3 on L + 1. Pages resident for other reasons (the air's, VsmMarkAir, which are not
    // propagated) failed the fallback: taps off their page read "no caster", and floor reflections in a closed bathhouse
    // showed sunlight through its walls (22 % of the image; a shadow ray there: 0.7 %) [measured, 2026-09-27]. Finest
    // level first: the direct view's own pages, the most accurate. None: a shadow ray (resident = false).
    [loop] for (int j = min(3, (int)k); j >= 0; --j)
    {
        const uint L = k - j;
        if (vsmEntry(r, vsmAbsPage(vsmAbsTexel(c, vsmLightSpaceAt(c, worldPos, L).xy, L)), L) == 0) continue;
        bool covered = true;
        if (L + 1 < VSM_LEVELS)
        {
            const int2 centre = vsmAbsPage(vsmAbsTexel(c, vsmLightSpaceAt(c, worldPos, L + 1).xy, L + 1));
            [loop] for (uint q = 0; q < 9 && covered; ++q)
                covered = vsmEntry(r, centre + int2((int)(q % 3) - 1, (int)(q / 3) - 1), L + 1) != 0;
        }
        if (!covered) continue;
        level = L;
        break;
    }
    resident = level != 0xFFFFFFFFu;
    if (!resident) return 1;
    uint path;
    // Opaque casters (height field) times the thin casters' transmittance (v1.26).
    return vsmSunVisibility(r, worldPos, normal, vsmTexel(level), c.tanSunRadius, c.searchTaps, c.filterTaps, path) *
           shadowSunTransmittanceAt(s, worldPos, footprint, footprint);
}

// shadowSunVisibilityAt in two steps, for callers that defer the penumbra filter to a pass of their own (R's reflection
// hits, ReflectionPenumbra.hlsl: the filter's 5 + 16 dependent taps run only on the hits whose region needs them, in
// dense waves, instead of stalling every wave of the hit shading that holds one). The same estimator, levels and inputs:
// visibility = { 1 lit | 0 umbra | shadowSunPenumbraDeferred(...) } x transmittance.
struct ShadowSunClassified
{
    bool resident;       // false: no level holds the point (the caller traces a shadow ray)
    bool penumbra;       // the filter is still to run (shadowSunPenumbraDeferred with k and reach)
    float visibility;    // lit / umbra: the visibility with the transmittance; penumbra: 0
    float transmittance; // the thin casters' transmittance (the penumbra's factor)
    uint k;              // the receiver's and the filter's level
    float reach;         // the filter disk's reach (vsmSunClassify)
};
ShadowSunClassified shadowSunClassifyAt(ShadowSrvs s, float3 worldPos, float3 normal, float footprint)
{
    ShadowSunClassified o;
    o.resident = o.penumbra = false;
    o.visibility = 1;
    o.transmittance = 1;
    o.k = 0;
    o.reach = 0;
    VsmResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    const uint k = vsmLevelForFootprint(c, footprint);
    uint level = 0xFFFFFFFFu;
    [loop] for (int j = min(3, (int)k); j >= 0; --j)  // (shadowSunVisibilityAt's residency rule)
    {
        const uint L = k - j;
        if (vsmEntry(r, vsmAbsPage(vsmAbsTexel(c, vsmLightSpaceAt(c, worldPos, L).xy, L)), L) == 0) continue;
        bool covered = true;
        if (L + 1 < VSM_LEVELS)
        {
            const int2 centre = vsmAbsPage(vsmAbsTexel(c, vsmLightSpaceAt(c, worldPos, L + 1).xy, L + 1));
            [loop] for (uint q = 0; q < 9 && covered; ++q)
                covered = vsmEntry(r, centre + int2((int)(q % 3) - 1, (int)(q / 3) - 1), L + 1) != 0;
        }
        if (!covered) continue;
        level = L;
        break;
    }
    o.resident = level != 0xFFFFFFFFu;
    if (!o.resident) return o;
    // vsmSunVisibility's steps at footprint = vsmTexel(level)
    const float texel = vsmTexel(level);
    const VsmReceiver rc = vsmMakeReceiver(c, worldPos, normal, vsmLevelForFootprint(c, texel));
    uint path;
    const uint cls = vsmSunClassify(r, rc, texel, c.tanSunRadius, o.k, o.reach, path);
    o.transmittance = shadowSunTransmittanceAt(s, worldPos, footprint, footprint);
    o.penumbra = cls != VSM_REGION_LIT && cls != VSM_REGION_UMBRA;
    o.visibility = cls == VSM_REGION_LIT ? o.transmittance : 0;
    return o;
}
// The penumbra filter of a point shadowSunClassifyAt left (penumbra = true): times its transmittance, the value
// shadowSunVisibilityAt returns there.
float shadowSunPenumbraDeferred(ShadowSrvs s, float3 worldPos, float3 normal, uint k, float reach)
{
    VsmResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    uint path;
    return vsmSunPenumbra(r, vsmMakeReceiver(c, worldPos, normal, k), k, reach, c.tanSunRadius, c.searchTaps, c.filterTaps, path);
}

// Sun visibility in [0, 1] at a point in the air (particle centres, FX request 20260926_FX_particle_render_pass 8e): no
// receiver surface (the receiver plane faces the sun, as the air walk's points), the level for 'footprint' (metres: the
// larger of the particle's radius and the pixel footprint at its depth) or the nearest resident one, finer levels first
// (up to three), then coarser (the air's pages, VsmMarkAir, are resident along every on-screen view ray), times the thin
// casters' transmittance. resident = false when no level holds the point (off-screen): the caller uses 1 or a ray.
float shadowSunVisibilityInAir(ShadowSrvs s, float3 worldPos, float footprint, out bool resident)
{
    VsmResources r;
    r.table = ResourceDescriptorHeap[s.pageTable];
    r.pool = ResourceDescriptorHeap[vsmAtlasSrv(s.constants)];  // the atlas (VsmConstants; ShadowSrvs.pool is not read)
    r.blocks = ResourceDescriptorHeap[s.blocks];
    r.searchBound = ResourceDescriptorHeap[s.searchBound];
    r.cbv = s.constants;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[s.constants];
    const uint k = vsmLevelForFootprint(c, footprint);
    uint level = 0xFFFFFFFFu;
    // Candidates k, k - 1, .., k - 3, then k + 1, .., VSM_LEVELS - 1: at most VSM_LEVELS + 3 probes (INTERFACES 3.6 cap).
    [loop] for (uint j = 0; j < VSM_LEVELS + 3 && level == 0xFFFFFFFFu; ++j)
    {
        const uint candidate = j < 4 ? k - j : k + (j - 3);
        if ((j < 4 && j > k) || candidate >= VSM_LEVELS) continue;
        if (vsmEntry(r, vsmAbsPage(vsmAbsTexel(c, vsmLightSpaceAt(c, worldPos, candidate).xy, candidate)), candidate) != 0) level = candidate;
    }
    resident = level != 0xFFFFFFFFu;
    if (!resident) return 1;
    uint path;
    return vsmSunVisibility(r, worldPos, c.level[level].lightZ, vsmTexel(level), c.tanSunRadius, c.searchTaps, c.filterTaps, path) *
           shadowSunTransmittanceAt(s, worldPos, max(footprint, vsmTexel(level)), max(footprint, vsmTexel(level)));
}

#endif
