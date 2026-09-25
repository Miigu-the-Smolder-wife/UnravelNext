// Public atmosphere lookups (INTERFACES_KO.md 5.6). Owner: S. Consumers: M (sky pixels, aerial perspective, sun),
// R (sky radiance for escaping rays, sun). Needs the frame constants (b1) of the calling view: sun direction,
// illuminance, colour and angular radius, camera position.
//
// Quantities (INTERFACES 8.3): radiance in nits (before exposure). The LUTs hold values per unit solar illuminance;
// these functions multiply by E_TOA * sunColor (g_sunIlluminance * g_sunColor).
//
// Exactness: the LUTs integrate the same model as the reference path tracer (Rayleigh, Mie HG, ozone, Lambertian
// ground) with the previous engine's quadratures; multiple scattering follows Hillaire 2020 (isotropic second-order
// series closed geometrically), which is the model's approximation and is recorded in S_STATUS_KO.md against the
// reference. Air shadowing by casters and local lights' in-scattering are in atmosphereAerial / atmosphereAirView
// (the air volume); atmosphereSkyRadiance / atmosphereSunRadiance / atmosphereSunIlluminance do not include caster shadows.
#ifndef UNX_ATMOSPHERE_HLSLI
#define UNX_ATMOSPHERE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

struct AtmosphereSrvs
{
    uint transmittance, multiScatter, skyView, aerial;
};

// Sky radiance seen from the camera in direction worldDir (unit), sun disk excluded: the far-field in-scattering of the
// whole ray to space or to the ground (including the ground's reflection below the horizon). Rays from other
// positions (R's escaping rays) use the camera's altitude; the sky varies with altitude on the scale height (1.2 km
// Mie), so this holds for points within tens of metres of the camera's altitude and is an approximation beyond.
float3 atmosphereSkyRadiance(AtmosphereSrvs s, float3 worldDir)
{
    const AtmosphereParams a = airParamsFromTexels(s.multiScatter);
    const float altitude = max(0.0, airAltitude(a, g_cameraPosition));
    const float3 up = airUp(a, g_cameraPosition);
    float2 coord;
    uint side;
    airSkyCoordinates(a, altitude, up, normalize(g_sunDirection), worldDir, coord, side);
    // Manual bilinear inside one half: never interpolates across the horizon.
    Texture2D<float4> t = ResourceDescriptorHeap[s.skyView];
    const uint half = a.skyViewSize.y / 2;
    const uint2 lo = min(uint2(coord), uint2(a.skyViewSize.x - 1, half - 1));
    const uint2 hi = min(lo + 1, uint2(a.skyViewSize.x - 1, half - 1));
    const float2 f = saturate(coord - float2(lo));
    const uint row = side * half;
    const float3 v00 = t.Load(int3(lo.x, row + lo.y, 0)).rgb, v10 = t.Load(int3(hi.x, row + lo.y, 0)).rgb;
    const float3 v01 = t.Load(int3(lo.x, row + hi.y, 0)).rgb, v11 = t.Load(int3(hi.x, row + hi.y, 0)).rgb;
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y) * (g_sunIlluminance * g_sunColor);
}

// Radiance of the solar disk seen from worldPos (uniform disk, INTERFACES 8.3): E_TOA * T(p -> sun) * colour /
// (pi sin^2 theta_s). Zero when the planet blocks the sun. Caster shadows are not included.
float3 atmosphereSunRadiance(AtmosphereSrvs s, float3 worldPos)
{
    const AtmosphereParams a = airParamsFromTexels(s.multiScatter);
    const float sinTheta = sin(g_sunAngularRadius);
    const float3 T = airSunTransmittance(a, s.transmittance, worldPos, normalize(g_sunDirection));
    return g_sunIlluminance * g_sunColor * T / (ATMO_PI * sinTheta * sinTheta);
}

// Solar illuminance arriving at worldPos on a surface facing the sun (lux): E_TOA * T(p -> sun) * colour.
float3 atmosphereSunIlluminance(AtmosphereSrvs s, float3 worldPos)
{
    const AtmosphereParams a = airParamsFromTexels(s.multiScatter);
    return g_sunIlluminance * g_sunColor * airSunTransmittance(a, s.transmittance, worldPos, normalize(g_sunDirection));
}

// Air between the main camera and the surface at screen uv (main view, [0,1]^2) and view-space depth linearDepth
// (Frame.hlsli linearDepth): in-scattered radiance (nits) and chromatic transmittance of everything in the air of the
// main view, from the air volume S's froxels() builds on the froxel grid (tile_px x depth_slices, FroxelIntegrate.hlsl):
// the atmosphere's single scattering (with the casters' shadows in the air, VSM) and multiple scattering, and the local
// lights' in-scattering by the air. Two trilinear fetches; the frame constants bound must be the main view's.
//  - Depth: nodes exponential in view depth to atmosphere.froxels.far_m (clamped beyond), interpolated linearly in depth
//    (hardware weight, 1/256 of a node step).
//  - Direction: the phase of the tile-centre ray, interpolated bilinearly across tiles: within 0.1 % of the pixel's own
//    Mie phase (g = 0.8, 0.67 deg tiles at 4K; S_STATUS_KO.md), Rayleigh exact to 1e-5.
void atmosphereAerial(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)
{
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.multiScatter];
    uint w, h, d;
    v.GetDimensions(w, h, d);
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const float farM = p.Load(int3(4, ph - 1, 0)).w;
    const float4 q8 = p.Load(int3(8, ph - 1, 0));
    const float tilePx = asuint(q8.z), nearM = q8.w;
    const float N = d / 3;  // nodes per part (S + 1)
    const float c = airNodeCoord(nearM, farM, N - 1, min(linearDepth, farM));
    const float2 xy = uv * float2(g_viewWidth, g_viewHeight) / (float2(w, h) * tilePx);
    const float4 a = v.SampleLevel(g_linearClamp, float3(xy, (c + 0.5) / d), 0);
    const float4 t = v.SampleLevel(g_linearClamp, float3(xy, (c + 0.5 + N) / d), 0);
    inscatter = a.rgb / g_exposure;  // stored pre-exposed
    transmittance = exp(-t.rgb);
}

// atmosphereAerial plus the unshadowed solar illuminance (lux) at the surface point (main view): the air volume's sun
// transmittance at that depth along the tile-centre ray (the pixel's own ray differs by less than a tile laterally).
// Three trilinear fetches; replaces atmosphereAerial + atmosphereSunIlluminance for main-view pixels.
void atmosphereAirView(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance, out float3 sunIlluminance)
{
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.multiScatter];
    uint w, h, d;
    v.GetDimensions(w, h, d);
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const float farM = p.Load(int3(4, ph - 1, 0)).w;
    const float4 q8 = p.Load(int3(8, ph - 1, 0));
    const float tilePx = asuint(q8.z), nearM = q8.w;
    const float N = d / 3;
    const float c = airNodeCoord(nearM, farM, N - 1, min(linearDepth, farM));
    const float2 xy = uv * float2(g_viewWidth, g_viewHeight) / (float2(w, h) * tilePx);
    const float4 a = v.SampleLevel(g_linearClamp, float3(xy, (c + 0.5) / d), 0);
    const float4 t = v.SampleLevel(g_linearClamp, float3(xy, (c + 0.5 + N) / d), 0);
    const float4 e = v.SampleLevel(g_linearClamp, float3(xy, (c + 0.5 + 2 * N) / d), 0);
    inscatter = a.rgb / g_exposure;
    transmittance = exp(-t.rgb);
    sunIlluminance = e.rgb * (g_sunIlluminance * g_sunColor);
}

#endif
