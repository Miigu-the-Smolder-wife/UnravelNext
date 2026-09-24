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
// reference. Air shadowing (terrain, casters) is not in these functions; the froxel volume carries it (Froxel.hlsli).
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
// (Frame.hlsli linearDepth): in-scattered radiance (nits, unshadowed) and chromatic transmittance. The frame constants
// bound must be the main view's (the volume is built for its frustum).
//  - Transmittance: the volume's optical depth interpolated linearly in depth (exact for a homogeneous segment).
//    (Bruneton's two-lookup difference of LUT optical depths was measured worse: its absolute LUT error dominates
//    short and near-horizontal segments; S_STATUS_KO.md.)
//  - In-scattering: I = K (1 - T) with K interpolated linearly in depth between nodes (exact for a homogeneous segment,
//    K does not depend on the density scale) and the exact Rayleigh / Mie phase of the pixel's direction.
// Depths beyond atmosphere.aerial_max_distance_m clamp to it. The composition with the froxel volume is in Froxel.hlsli.
void atmosphereAerial(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)
{
    const AtmosphereParams a = airParamsFromTexels(s.multiScatter);
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    const float S = a.aerialSlices, N = S + 1, D = 4 * N, half = 0.5 / D;
    const float z = min(max(linearDepth, 0.0), a.aerialMaxDistance);
    const float n0 = clamp(floor(sqrt(z / a.aerialMaxDistance) * S), 0.0, S - 1);
    const float za = a.aerialMaxDistance * (n0 / S) * (n0 / S), zb = a.aerialMaxDistance * ((n0 + 1) / S) * ((n0 + 1) / S);
    const float w = saturate((z - za) / (zb - za));
    // Node n of component c sits at depth coordinate (c N + n + 0.5) / 4N.
    const float3 kR = lerp(v.SampleLevel(g_linearClamp, float3(uv, n0 / D + half), 0).rgb, v.SampleLevel(g_linearClamp, float3(uv, (n0 + 1) / D + half), 0).rgb, w);
    const float3 kM = lerp(v.SampleLevel(g_linearClamp, float3(uv, (N + n0) / D + half), 0).rgb, v.SampleLevel(g_linearClamp, float3(uv, (N + n0 + 1) / D + half), 0).rgb, w);
    const float3 kS = lerp(v.SampleLevel(g_linearClamp, float3(uv, (2 * N + n0) / D + half), 0).rgb, v.SampleLevel(g_linearClamp, float3(uv, (2 * N + n0 + 1) / D + half), 0).rgb, w);
    const float3 tau = lerp(v.SampleLevel(g_linearClamp, float3(uv, (3 * N + n0) / D + half), 0).rgb, v.SampleLevel(g_linearClamp, float3(uv, (3 * N + n0 + 1) / D + half), 0).rgb, w);
    transmittance = exp(-tau);
    const float3 dir = airViewDirection(uv);
    const float nu = dot(dir, normalize(g_sunDirection));
    const float3 K = kR * airRayleighPhase(nu) + kM * airMiePhase(nu, a.mieG) + kS;
    inscatter = K * airOneMinusExp(tau) * (g_sunIlluminance * g_sunColor);
}

#endif
