// Light functions in the surface cache's direct light (A8: IES profile, cookie, gobo, intensity and colour keys, flicker;
// Passes/Lights/LightFunction.hlsli). The opaque shading, the coverage layer, the air and the ray hits multiply a
// punctual light by its function toward the receiver; a cached direct-light value takes it too, or the bounce of a gobo
// spotlight is that of a plain one. The table is the frame's, named by the ray scene's light data (RayScene: its
// header's byte 80, written by r.lights.functions; the passes declare it with RayScene::declareTraversal).
// A function that changes with time (a rotating profile, intensity or colour keys, flicker) makes the stored value a
// sample of the moment it was stored at: the stores mark what such a light lights for the earliest refresh
// (scLightFunctionAnimated; cards: CL_PAGE_ANIMATED in the page's light state, CardSelect.hlsl; cells: the head's
// feedback bit, SurfaceCacheUpdate.hlsl).
#ifndef UNX_SURFACE_CACHE_LIGHT_FUNCTION_HLSLI
#define UNX_SURFACE_CACHE_LIGHT_FUNCTION_HLSLI
#include "Scene.hlsli"
#include "Passes/Lights/LightFunction.hlsli"

// The frame's function table (LIGHT_FUNCTION_NONE: no light has a function). lightData: RtSceneSrvs::pad.
uint scLightFunctionTable(uint lightData)
{
    if (lightData == 0xFFFFFFFFu) return LIGHT_FUNCTION_NONE;
    ByteAddressBuffer b = ResourceDescriptorHeap[lightData];
    return b.Load(80);
}

// The multiplier of light 'light' (g, a punctual light: area lights have no function) toward a cached point.
// size: the point's extent (a card texel, a cell; m) - its angle from the light picks the image's level.
float3 scLightFunction(uint table, GpuLight g, uint light, float3 position, float size)
{
    if (table == LIGHT_FUNCTION_NONE || lightType(g) > LIGHT_SPOT) return 1;
    const float3 d = position - g.position;
    const float dist = length(d);
    if (!(dist > 1e-6)) return 1;
    return lightFunction(table, light, g.forward, g.right, d / dist, size / dist, g_time);
}

// Whether the light's function changes with time (the table's record, LightFunction.hlsli: a profile that rotates, more
// than one intensity or colour key, flicker).
bool scLightFunctionAnimated(uint table, uint light)
{
    if (table == LIGHT_FUNCTION_NONE) return false;
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    if (light >= b.Load(0)) return false;
    const uint o = b.Load(16 + 4 * light);
    if (o == 0) return false;
    const uint profile = b.Load(o);
    const float rotationSpeed = asfloat(b.Load(o + 40));
    const uint4 keys = b.Load4(o + 48);     // intensity key count, period, offset, colour key count
    const float flickerDepth = asfloat(b.Load(o + 72));
    const uint flickerOctaves = b.Load(o + 80);
    return (profile != 0 && rotationSpeed != 0) || keys.x > 1 || keys.w > 1 || (flickerDepth > 0 && flickerOctaves != 0);
}

#endif
