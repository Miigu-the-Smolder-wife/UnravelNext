// Public atmosphere lookups (INTERFACES_KO.md 5.6). Owner: S. Consumers: M (sky pixels, aerial perspective, sun),
// R (sky radiance for escaping rays, sun). Needs the frame constants (b1) of the calling view: sun direction,
// illuminance, colour and angular radius, camera position.
//
// Quantities (INTERFACES 8.3): radiance in nits (before exposure). The LUTs hold values per unit solar illuminance;
// these functions multiply by E_TOA * sunColor (g_sunIlluminance * g_sunColor).
//
// Exactness: the LUTs integrate the same model as the reference path tracer (Rayleigh, Mie HG, ozone, Lambertian
// ground) with the previous engine's quadratures; multiple scattering is the exact source table J_ms (orders iterated,
// S_STATUS_KO.md 8; checked against the reference path tracer's scattering orders). Air shadowing by casters and local lights' in-scattering are in atmosphereAerial / atmosphereAirView
// (the air volume); atmosphereSkyRadiance / atmosphereSunRadiance / atmosphereSunIlluminance do not include caster shadows.
#ifndef UNX_ATMOSPHERE_HLSLI
#define UNX_ATMOSPHERE_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Passes/Atmosphere/CloudCommon.hlsli"  // definitions only (the dome's parameterization); no code unless called
#include "Passes/Atmosphere/FogVolume.hlsli"    // the height fog's volume: the air lookups below take it with the air

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
    const AtmosphereParams a = airParamsFromTexels(s.transmittance);
    const float altitude = max(0.0, airAltitude(a, g_cameraPosition));
    const float3 up = airUp(a, g_cameraPosition);
    float2 coord;
    uint side;
    airSkyCoordinates(a, altitude, up, normalize(g_sunDirection), worldDir, coord, side);
    // Manual bilinear inside one half: never interpolates across the horizon. Three parts (SkyView.hlsl): multiple
    // scattering + ground, and the Rayleigh and Mie single scattering without phase, times this direction's phases.
    Texture2D<float4> t = ResourceDescriptorHeap[s.skyView];
    const uint half = a.skyViewSize.y / 2;
    const uint2 lo = min(uint2(coord), uint2(a.skyViewSize.x - 1, half - 1));
    const uint2 hi = min(lo + 1, uint2(a.skyViewSize.x - 1, half - 1));
    const float2 f = saturate(coord - float2(lo));
    float3 part[3];
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const uint row = side * half + k * a.skyViewSize.y;
        const float3 v00 = t.Load(int3(lo.x, row + lo.y, 0)).rgb, v10 = t.Load(int3(hi.x, row + lo.y, 0)).rgb;
        const float3 v01 = t.Load(int3(lo.x, row + hi.y, 0)).rgb, v11 = t.Load(int3(hi.x, row + hi.y, 0)).rgb;
        part[k] = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
    }
    const float nu = dot(normalize(worldDir), normalize(g_sunDirection));
    return (part[0] + part[1] * airRayleighPhase(nu) + part[2] * airMiePhase(nu, a.mieG)) * (g_sunIlluminance * g_sunColor);
}

// Radiance of the solar disk seen from worldPos (uniform disk, INTERFACES 8.3): E_TOA * T(p -> sun) * colour /
// (pi sin^2 theta_s). Zero when the planet blocks the sun. Caster shadows are not included.
float3 atmosphereSunRadiance(AtmosphereSrvs s, float3 worldPos)
{
    const AtmosphereParams a = airParamsFromTexels(s.transmittance);
    const float sinTheta = sin(g_sunAngularRadius);
    const float3 T = airSunTransmittance(a, s.transmittance, worldPos, normalize(g_sunDirection));
    return g_sunIlluminance * g_sunColor * T / (ATMO_PI * sinTheta * sinTheta);
}

// Solar illuminance arriving at worldPos on a surface facing the sun (lux): E_TOA * T(p -> sun) * colour.
float3 atmosphereSunIlluminance(AtmosphereSrvs s, float3 worldPos)
{
    const AtmosphereParams a = airParamsFromTexels(s.transmittance);
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
    N = (depth - 1) / 3;  // nodes per part (S + 1); then the sky correction and the media's optical depth to far_m
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

// The air of the main view's pixel at uv up to view depth linearDepth (see atmosphereAerial). The volume is read at the
// depth itself, or, when the pixel's ray passes below the model's surface first (terrain below the origin's altitude:
// the reference takes the surface air there, homogeneous), at the depth where it crosses the surface, and the rest of
// the path is integrated with the surface air (exact transmittance), the pixel's own phase, and the sun transmittance and
// multiple scattering of the lifted points at 8 midpoints. No tile interpolation spans the crossing, at any resolution.
void airViewLookup(AtmosphereSrvs s, float2 uv, float linearDepth, bool wantSun, out float3 inscatter, out float3 transmittance, out float3 sunTransmittance)
{
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.transmittance];
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const float bottom = p.Load(int3(0, ph - 1, 0)).x;
    // The pixel's ray, its distance per unit view depth and where it enters the model's surface (if before the depth).
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float4 q = mul(g_invViewProj, float4(ndc, 1, 1));
    const float3 dir = normalize(q.xyz / q.w - g_cameraPosition);
    const float toRay = 1.0 / max(dot(dir, airViewForward()), 1e-4);
    // Planar reflection views: the air starts at the mirror (airViewStart); the surface crossing is searched from there.
    const float tStart = min(airViewStart(g_clipPlane, g_cameraPosition, dir), 1.0e30);
    const float3 o = g_cameraPosition + dir * tStart;
    const float h2 = dot(o, o) + 2 * bottom * o.y;  // (r^2 - R^2) of the path's start
    float kink = 3.0e38;                            // ray distance to the surface crossing
    if (h2 <= 0) kink = tStart;
    else
    {
        const float b = dot(o, dir) + bottom * dir.y, disc = b * b - h2;
        if (disc >= 0 && b < 0) kink = tStart + h2 / (-b + sqrt(disc));  // nearer root, stable form
    }
    kink = max(kink, AIR_VIEW_START_M);  // (the surface air's closed-form tail starts where the view's air does)
    const float tDepth = linearDepth * toRay;
    const bool lifted = kink < tDepth;
    float N, d;
    const float4 t = airVolumeCoord(v, p, uv, lifted ? kink / toRay : linearDepth, N, d);
    const float4 a = v.SampleLevel(g_linearClamp, t.xyz, 0);
    const float4 od = v.SampleLevel(g_linearClamp, float3(t.x, t.w, t.z + N / d), 0);
    // Stored pre-exposed as L / (1 - T), blended across tiles; times the pixel's own 1 - T (FroxelIntegrate.hlsl).
    inscatter = a.rgb / g_exposure * airOneMinusExp(od.rgb);
    transmittance = exp(-od.rgb);
    sunTransmittance = 0;
    if (wantSun || lifted) sunTransmittance = v.SampleLevel(g_linearClamp, float3(t.x, t.w, t.z + 2 * N / d), 0).rgb;
    if (!lifted) return;
    const AtmosphereParams ap = airParamsFromTexels(s.transmittance);
    const AirCoefficients c = airCoefficients(ap, 0.0);
    const float3 sun = normalize(g_sunDirection);
    const float nu = dot(dir, sun);
    const float3 phase = c.rayleigh * airRayleighPhase(nu) + c.mie * airMiePhase(nu, ap.mieG);
    const float L = max(min(tDepth, ap.froxelFarM * toRay) - kink, 0.0);
    // Homogeneous surface air: exact transmittance; the sun and multiple scattering of the lifted points (their surface
    // normal turns along the path) at LIFTED_STEPS midpoints.
    const uint LIFTED_STEPS = 8;
    const float dt = L / LIFTED_STEPS;
    const float3 step = airIntegral(c.extinction, dt), decay = exp(-c.extinction * dt);
    [unroll] for (uint k = 0; k < LIFTED_STEPS; ++k)
    {
        const float3 x = airLiftToSurface(ap, g_cameraPosition + dir * (kink + (k + 0.5) * dt));
        const float3 source = phase * airSunTransmittance(ap, s.transmittance, x, sun) + (c.rayleigh + c.mie) * airMultipleScattering(ap, s.multiScatter, x, dir, sun);
        inscatter += transmittance * source * step * (g_sunIlluminance * g_sunColor);
        transmittance *= decay;
    }
    // The sun at the (lifted) surface point itself.
    if (wantSun) sunTransmittance = airSunTransmittance(ap, s.transmittance, airLiftToSurface(ap, g_cameraPosition + dir * min(tDepth, ap.froxelFarM * toRay)), sun);
}

// ---- B5 volumetric clouds (CloudSystem.cpp): the main view's cloud layer, marched at quarter resolution along the whole
// view ray with the air in front of it folded in (CloudMarch.hlsl: rgb = in(0, d_c)(1 - T_c) + T_air(0, d_c) L_c, a = T_c,
// d_c the extinction-weighted cloud distance). A pixel whose surface (or the sky) lies beyond d_c: in' = rgb + T_c in,
// T' = T_c T; a surface nearer than d_c is in front of the cloud and unchanged. Applied to sky pixels
// (atmosphereSkyRadianceClouded, ShadeSky);
// surfaces beyond the clouds and the cloud shadow on surfaces come from S passes, not from code inlined into M's shading
// kernels (their DXIL is at the 200 KB limit). Secondary views: no clouds yet (A14).
uint airCloudRecordSrv(AtmosphereSrvs s)
{
    if (g_viewKind != 0) return 0;
    Texture2D<float4> p = ResourceDescriptorHeap[s.transmittance];
    uint pw, ph;
    p.GetDimensions(pw, ph);
    return asuint(p.Load(int3(10, ph - 1, 0)).z);  // AtmosphereParams.clouds.x (SRV + 1; 0 = none)
}
void airApplyClouds(AtmosphereSrvs s, float2 uv, float surfaceM, inout float3 inscatter, inout float3 transmittance)
{
    const uint r = airCloudRecordSrv(s);
    if (r == 0) return;
    ByteAddressBuffer b = ResourceDescriptorHeap[r - 1];
    const uint2 srvs = b.Load2(88);  // CloudRecord layerSrv, distanceSrv
    Texture2D<float4> layer = ResourceDescriptorHeap[srvs.x];
    Texture2D<float> dist = ResourceDescriptorHeap[srvs.y];
    if (surfaceM <= dist.SampleLevel(g_linearClamp, uv, 0) * 1000) return;
    const float4 v = layer.SampleLevel(g_linearClamp, uv, 0);
    inscatter = v.rgb + v.a * inscatter;
    transmittance *= v.a;
}

// The far-field sky with the cloud layer in front, for a direction from the camera (R's escaping GI and reflection rays;
// not called by M's kernels): the sky dome (CloudMarch.hlsl mode 3: the layer with the air in front folded in) over
// atmosphereSkyRadiance. Without clouds: atmosphereSkyRadiance.
float3 atmosphereSkyRadianceCloudy(AtmosphereSrvs s, float3 worldDir)
{
    const float3 sky = atmosphereSkyRadiance(s, worldDir);
    Texture2D<float4> p = ResourceDescriptorHeap[s.transmittance];
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const uint r = asuint(p.Load(int3(10, ph - 1, 0)).z);
    if (r == 0) return sky;
    ByteAddressBuffer b = ResourceDescriptorHeap[r - 1];
    Texture2D<float4> dome = ResourceDescriptorHeap[b.Load(76)];  // CloudRecord skySrv
    uint dw, dh;
    dome.GetDimensions(dw, dh);
    float2 uv = cloudDomeUv(worldDir);
    uv.y = clamp(uv.y, 0.5 / dh, 1 - 0.5 / dh);  // wrap in azimuth only
    const float4 v = dome.SampleLevel(g_linearWrap, uv, 0);
    return v.rgb + v.a * sky;
}

// Sky pixels of a view with an air volume (ShadeSky; INTERFACES 5.6): the far-field sky (atmosphereSkyRadiance) plus
// the volume's sky correction at the pixel (bilinear across tiles, like the surface lookups): the local lights'
// in-scattering by the air (a street lamp's glow against the night sky) and the single scattering the casters' shadows
// remove from it (shafts from a ridge in front of a low sun), both along the tile rays to atmosphere.froxels.far_m.
// Beyond far_m (65 km) the air is the LUT's (no casters or local lights reach it). uv: the pixel centre over the view.
// Without a volume (s.aerial = UNX_NONE) it is atmosphereSkyRadiance.
float3 atmosphereSkyRadianceView(AtmosphereSrvs s, float3 worldDir, float2 uv)
{
    float3 radiance = atmosphereSkyRadiance(s, worldDir);
    if (s.aerial == 0xFFFFFFFFu) return radiance;
    Texture3D<float4> v = ResourceDescriptorHeap[s.aerial];
    Texture2D<float4> p = ResourceDescriptorHeap[s.transmittance];
    uint w, h, depth;
    v.GetDimensions(w, h, depth);
    uint pw, ph;
    p.GetDimensions(pw, ph);
    const float tilePx = asuint(p.Load(int3(8, ph - 1, 0)).z);
    const float2 cell = uv * float2(g_viewWidth, g_viewHeight) / tilePx - 0.5;
    const float2 t = (clamp(cell, 0.0, float2(w, h) - 1) + 0.5) / float2(w, h);
    // Slices 3 (S + 1) and 3 (S + 1) + 1 (FroxelIntegrate.hlsl): the sky correction, and the particle media's optical
    // depth to far_m (the far-field sky seen through smoke and fire).
    const float N = (depth - 1) / 3;
    const float3 correction = v.SampleLevel(g_linearClamp, float3(t, (3 * N + 0.5) / depth), 0).rgb / g_exposure;
    const float3 media = v.SampleLevel(g_linearClamp, float3(t, (3 * N + 1.5) / depth), 0).rgb;
    return max(radiance * exp(-media) + correction, 0.0);
}

// atmosphereSkyRadianceView with the cloud layer (B5) in front of the sky: for ShadeSky's sky pixels (one call site; the
// other readers stay without it so M's large kernels keep their DXIL size).
// behind: the layer's transmittance at the pixel - what lies behind it (the sun's disk, the moon, the stars) takes it.
float3 atmosphereSkyRadianceClouded(AtmosphereSrvs s, float3 worldDir, float2 uv, out float3 behind)
{
    float3 sky = atmosphereSkyRadianceView(s, worldDir, uv);
    behind = 1;
    airApplyClouds(s, uv, 3.0e38, sky, behind);
    return sky;
}

// Air between the main camera and the surface at screen uv (main view, [0,1]^2) and view-space depth linearDepth
// (Frame.hlsli linearDepth): in-scattered radiance (nits) and chromatic transmittance of everything in the air of the
// main view, from the air volume S's froxels() builds on the froxel grid (tile_px x depth_slices, FroxelIntegrate.hlsl):
// the atmosphere's single scattering (with the casters' shadows in the air, VSM) and multiple scattering, and the local
// lights' in-scattering by the air. Two trilinear fetches (plus the closed-form tail of a ray below the model's surface,
// airViewLookup); the frame constants bound must be the main view's.
//  - Depth: nodes exponential in view depth to atmosphere.froxels.far_m (clamped beyond), interpolated linearly in depth
//    (hardware weight, 1/256 of a node step).
//  - Across tiles: bilinear (airVolumeCoord; optical depth across a row lifted below the surface: in altitude).
//  - Direction: the phase of the tile-centre ray, interpolated across tiles: within 0.1 % of the pixel's own Mie phase
//    (g = 0.8, 0.67 deg tiles at 4K; S_STATUS_KO.md), Rayleigh exact to 1e-5.
// With the height fog on (FogVolume.hlsli; the frame constants' g_fog) both carry the fog to that depth too.
void atmosphereAerial(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance)
{
    float3 sunT;
    airViewLookup(s, uv, linearDepth, false, inscatter, transmittance, sunT);
#ifndef UNX_AIR_WITHOUT_FOG
    fogOverAir(uv, linearDepth, inscatter, transmittance);
#endif
}

// atmosphereAerial plus the unshadowed solar illuminance (lux) at the surface point (main view): the air volume's sun
// transmittance at that depth along the tile rays (the pixel's own ray differs by less than a tile laterally).
// Three trilinear fetches; replaces atmosphereAerial + atmosphereSunIlluminance for main-view pixels.
void atmosphereAirView(AtmosphereSrvs s, float2 uv, float linearDepth, out float3 inscatter, out float3 transmittance, out float3 sunIlluminance)
{
    float3 sunT;
    airViewLookup(s, uv, linearDepth, true, inscatter, transmittance, sunT);
    sunIlluminance = sunT * (g_sunIlluminance * g_sunColor);
#ifndef UNX_AIR_WITHOUT_FOG  // (a kernel at the size limit leaves the fog out: FxLayerSetup's ML = 0, GIV = 0 variant)
    fogOverAir(uv, linearDepth, inscatter, transmittance);
#endif
}

#endif
