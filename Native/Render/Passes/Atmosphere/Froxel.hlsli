// Public froxel lookups (INTERFACES_KO.md 5.6, 7.4). Owner: S. Consumers: M (shading), S (shadow slots).
// Fill FroxelSrvs from FrameResources: lights = lightIndices = SRV of froxelLights (one raw buffer, FroxelCommon.hlsli),
// scattering = SRV of froxels (Texture3D, RGBA16F). The lookups need the main view's frame constants (froxelScattering
// takes uv over the main view).
//
// froxelScattering returns, for the camera -> view depth segment, what the air perspective of Atmosphere.hlsli does not
// hold: rgb = the sun's in-scattering the air does NOT receive where casters shadow it (negative; the VSM seen from
// the air, i.e. god rays) plus the in-scattering of the local lights in the air, in nits; a = transmittance of local
// media (1: scenes v1 carry no local media). Composition for a surface of radiance L at that depth:
//     L_out = (L * T_air + L_air) * a + rgb        (T_air, L_air from atmosphereAerial)
// which is exact while the local media are optically thin next to the air (a = 1 in v1). Depths beyond
// atmosphere.froxels.far_m clamp to it (the shafts' contribution beyond is not held).
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

// Segment camera -> linearDepth through screen uv (see the composition above).
float4 froxelScattering(FroxelSrvs f, float2 uv, float linearDepth)
{
    const FroxelGrid g = froxelGrid(f.lights);
    Texture3D<float4> v = ResourceDescriptorHeap[f.scattering];
    // Node n (n = 1..S) is the value at the far end of slice n - 1 (depth z_n), stored in volume slice n - 1; node 0 is
    // the camera (zero in-scattering, transmittance 1). Slice 0 starts at the camera. Linear in view depth between nodes.
    const float w = clamp(froxelSliceCoord(g, linearDepth), 0.0, float(g.slices));
    const float s1 = clamp(floor(w) + 1, 1.0, float(g.slices));
    const float s0 = s1 - 1;
    const float z0 = s0 < 0.5 ? 0.0 : froxelSliceDepth(g, s0), z1 = froxelSliceDepth(g, s1);
    const float t = saturate((min(linearDepth, g.farM) - z0) / max(z1 - z0, 1e-6));
    // Volume texel x holds the tile centre; the grid covers gridX * tilePx >= view width pixels.
    const float2 u = uv * float2(g_viewWidth, g_viewHeight) / (float2(g.gridX, g.gridY) * g.tilePx);
    const float4 b = v.SampleLevel(g_linearClamp, float3(u, (s1 - 0.5) / g.slices), 0);
    const float4 a = s0 < 0.5 ? float4(0, 0, 0, 1) : v.SampleLevel(g_linearClamp, float3(u, (s0 - 0.5) / g.slices), 0);
    const float4 r = lerp(a, b, t);
    return float4(r.rgb / g_exposure, r.a);  // the volume is stored pre-exposed (FroxelIntegrate.hlsl)
}

#endif
