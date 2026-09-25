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

// Texture coordinates of (uv, linearDepth) in the air volume (main view). Across tiles the volume holds the air of the
// tile-centre rays, interpolated bilinearly in screen position (in-scattering follows the ray's direction: .xyz).
// Where exactly one of the two neighbouring tile rows has passed below the model's surface at that depth (air lifted to
// the surface: terrain below the origin's altitude), optical depth and sun transmittance follow the air the ray is in,
// so their vertical weight is taken in the rays' altitude at that depth (.w = their vertical coordinate).
float4 airVolumeCoord(Texture3D<float4> v, Texture2D<float4> p, float2 uv, float linearDepth, out float N, out float d)
{
    uint w, h, depth;
    v.GetDimensions(w, h, depth);
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const float bottom = p.Load(int3(0, ph - 1, 0)).x;
    const float farM = p.Load(int3(4, ph - 1, 0)).w;
    const float4 q8 = p.Load(int3(8, ph - 1, 0));
    const float tilePx = asuint(q8.z), nearM = q8.w;
    N = depth / 3;  // nodes per part (S + 1)
    d = depth;
    const float z = min(linearDepth, farM);
    const float c = airNodeCoord(nearM, farM, N - 1, z);
    const float2 pixel = uv * float2(g_viewWidth, g_viewHeight);
    const float2 cell = pixel / tilePx - 0.5;  // continuous tile coordinate (tile centres at integers)
    const float r0 = clamp(floor(cell.y), 0.0, float(h) - 1), r1 = min(r0 + 1, float(h) - 1);
    const float fy = saturate(cell.y - r0);
    float fyAir = fy;
    if (r1 > r0)
    {
        // Altitude (lifted: not below 0) at view depth z of the rays through this pixel column at the two row centres
        // and through the pixel.
        const float3 rays[3] = { float3(pixel.x, (r0 + 0.5) * tilePx, 0), float3(pixel.x, (r1 + 0.5) * tilePx, 0), float3(pixel, 0) };
        float alt[3];
        [unroll] for (uint i = 0; i < 3; ++i)
        {
            const float2 ndc = float2(rays[i].x / g_viewWidth * 2 - 1, 1 - rays[i].y / g_viewHeight * 2);
            const float4 q = mul(g_invViewProj, float4(ndc, 1, 1));  // device depth 1 = view depth g_nearPlane
            const float3 x = g_cameraPosition + (q.xyz / q.w - g_cameraPosition) * (z / g_nearPlane);
            const float h2 = dot(x, x) + 2 * bottom * x.y;
            alt[i] = max(0.0, h2 / (sqrt(max(0.0, bottom * bottom + h2)) + bottom));
        }
        // Only across the kink (exactly one row lifted): elsewhere the screen weight is the linear one.
        if ((alt[0] <= 0) != (alt[1] <= 0)) fyAir = saturate((alt[2] - alt[0]) / (alt[1] - alt[0]));
    }
    const float x = (clamp(cell.x, 0.0, float(w) - 1) + 0.5) / w;
    return float4(x, (r0 + fy + 0.5) / h, (c + 0.5) / d, (r0 + fyAir + 0.5) / h);
}

// Air between the main camera and the surface at screen uv (main view, [0,1]^2) and view-space depth linearDepth
// (Frame.hlsli linearDepth): in-scattered radiance (nits) and chromatic transmittance of everything in the air of the
// main view, from the air volume S's froxels() builds on the froxel grid (tile_px x depth_slices, FroxelIntegrate.hlsl):
// the atmosphere's single scattering (with the casters' shadows in the air, VSM) and multiple scattering, and the local
// lights' in-scattering by the air. Two trilinear fetches; the frame constants bound must be the main view's.
//  - Depth: nodes exponential in view depth to atmosphere.froxels.far_m (clamped beyond), interpolated linearly in depth
//    (hardware weight, 1/256 of a node step).
//  - Across tiles: bilinear (airVolumeCoord; optical depth across a row lifted below the surface: in altitude).
//  - Direction: the phase of the tile-centre ray, interpolated across tiles: within 0.1 % of the pixel's own Mie phase
//    (g = 0.8, 0.67 deg tiles at 4K; S_STATUS_KO.md), Rayleigh exact to 1e-5.
void atmosphereAerial(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)
{
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.multiScatter];
    float N, d;
    const float4 t = airVolumeCoord(v, p, uv, linearDepth, N, d);
    const float4 a = v.SampleLevel(g_linearClamp, t.xyz, 0);
    const float4 o = v.SampleLevel(g_linearClamp, float3(t.x, t.w, t.z + N / d), 0);
    inscatter = a.rgb / g_exposure;  // stored pre-exposed
    transmittance = exp(-o.rgb);
}

// atmosphereAerial plus the unshadowed solar illuminance (lux) at the surface point (main view): the air volume's sun
// transmittance at that depth along the tile rays (the pixel's own ray differs by less than a tile laterally).
// Three trilinear fetches; replaces atmosphereAerial + atmosphereSunIlluminance for main-view pixels.
void atmosphereAirView(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance, out float3 sunIlluminance)
{
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.multiScatter];
    float N, d;
    const float4 t = airVolumeCoord(v, p, uv, linearDepth, N, d);
    const float4 a = v.SampleLevel(g_linearClamp, t.xyz, 0);
    const float4 o = v.SampleLevel(g_linearClamp, float3(t.x, t.w, t.z + N / d), 0);
    const float4 e = v.SampleLevel(g_linearClamp, float3(t.x, t.w, t.z + 2 * N / d), 0);
    inscatter = a.rgb / g_exposure;
    transmittance = exp(-o.rgb);
    sunIlluminance = e.rgb * (g_sunIlluminance * g_sunColor);
}

#endif
