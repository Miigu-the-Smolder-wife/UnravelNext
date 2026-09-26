// Water fragment radiance (track W; render A's join point 2026-09-26): the radiance a water record contributes at a
// pixel, called by W's interior water pass for every water pixel and by M's coverage composition for the water records
// of edge pixels (COV_FLAG_WATER_EDGE), so both use one shading function. The composition weights it by area x
// unoccluded share like any record.
//   pixel       the pixel (x, y)
//   viewDepth   the record's linear view depth (m)
//   normalWorld the surface normal at the record (world space, unit, facing the viewer's side)
//   slot, tri   the record's triangle stream slot and triangle index (V's vis id; "triangle" is an HLSL keyword)
//   waterSrv    SRV index of the frame's water parameter buffer (FrameResources, written by W's waterGeometry/water
//               passes): the refraction source copy, band A depth, the medium (sigma_a RGB, sigma_s, g), the reflection
//               path results and the camera.
// Status: the interface is fixed with render A (INTERFACES v1.66). The body (Fresnel with WaterShading.hlsli, the
// screen-space refraction search and its validity test, absorption along the refracted path, the reflection path) is
// filled once the water surface's exact position is decided (Requests/20260926_W_ocean_patch_stream.md 2b and the
// coordination session's design item); until then there are no water records in a frame and this is never called.
#ifndef UNX_WATER_FRAGMENT_HLSLI
#define UNX_WATER_FRAGMENT_HLSLI
#include "WaterShading.hlsli"

// Water parameter buffer layout (raw, 16-byte rows). Row 0: refraction source SRV, band A depth SRV, reflection SRV,
// flags; row 1: sigma_a RGB (1/m), sigma_s (1/m); row 2: phase g, IOR, 0, 0; rows 3..: reserved for the camera.
#define WATER_PARAM_SOURCES 0u
#define WATER_PARAM_MEDIUM 16u
#define WATER_PARAM_PHASE 32u

float3 waterFragmentRadiance(uint2 pixel, float viewDepth, float3 normalWorld, uint slot, uint tri, uint waterSrv)
{
    return float3(0, 0, 0);  // no water records exist yet (see Status); render A's composition test drives this signature
}
#endif
