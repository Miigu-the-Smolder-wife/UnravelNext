// Public froxel lookups (INTERFACES_KO.md 5.6, 7.4). Owner: S. Consumers: M (shading), S (shadow slots).
// Fill FroxelSrvs from FrameResources: lights = lightIndices = SRV of froxelLights (one raw buffer, FroxelCommon.hlsli),
// scattering = SRV of froxels (Texture3D, RGBA16F). The lookups need the main view's frame constants (froxelScattering
// takes uv over the main view).
//
// froxelScattering: the air's shadows and local lights are part of atmosphereAerial / atmosphereAirView (one air volume
// on the froxel grid, Atmosphere.hlsli); this function returns the neutral (0, 0, 0, 1) and is to be removed from
// INTERFACES 5.6 (Docs/Design/Requests/20260925_S_air_volume.md).
#ifndef UNX_FROXEL_HLSLI
#define UNX_FROXEL_HLSLI
#include "Bindless.hlsli"
#include "Passes/Atmosphere/FroxelCommon.hlsli"

struct FroxelSrvs
{
    uint lights, lightIndices, scattering, pad;
};

// Light list of the froxel holding pixel 'pixel' (main view) at view depth linearDepth: (first entry, count).
uint2 froxelLightRange(FroxelSrvs f, uint2 pixel, float linearDepth)
{
    const FroxelGrid g = froxelGrid(f.lights);
    const uint2 tile = min(pixel / g.tilePx, uint2(g.gridX - 1, g.gridY - 1));
    ByteAddressBuffer b = ResourceDescriptorHeap[f.lights];
    const uint h = b.Load(g.headerBase + froxelIndex(g, tile, froxelSlice(g, linearDepth)) * 4);
    return uint2(h >> 6, h & 63u);
}

// Scene light index of list entry i.
uint froxelLight(FroxelSrvs f, uint i)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[f.lightIndices];
    const uint indexBase = b.Load(36);  // FroxelGrid::indexBase
    const uint w = b.Load(indexBase + (i >> 1) * 4);
    return (i & 1) ? (w >> 16) : (w & 0xFFFFu);
}

// Deprecated (see above): neutral.
float4 froxelScattering(FroxelSrvs f, float2 uv, float linearDepth) { return float4(0, 0, 0, 1); }

#endif
