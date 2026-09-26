// Water surface shading, stage 1 (FEATURES_GAME 1.3 (a) and 1.9; B8 fluids, W2 small water, later the sea): the exposed
// linear radiance, with aerial perspective, that a water surface sample sends to the camera. Used by W's interior pass
// (the water layer's full-coverage pixels, written into the shaded colour) and record pass (the layer's edge records,
// ViewResources::coverageRecordRadiance), so both shade with one function.
//   P    the sample: the pixel's view ray meets the water triangle (stream slot, triangle) at P, normal n (the vertex
//        normals interpolated; oriented out of the water).
//   Reflection  F (R_p of the exact unpolarised dielectric Fresnel, waterFresnel) x (the sun's disk through the surface
//        roughness's GGX lobe, shadowed by S's VSM + the GI cache's radiance of the mirror lobe) - the glass composite's
//        terms until R's reflection jobs cover the water layer (request R-1).
//   Transmission  the refracted ray (exact Snell) marched in screen space through band A's depth: where it meets band A
//        (H, refined by bisection) the light behind is the shaded band A radiance there, the water path d = |H - P| is
//        exact, and the transmitted radiance is (1 - F) exp(-sigma_a d) L_surface(H) / n^2 (radiance leaving water for
//        air, WaterShading.hlsli). sigma_a: pure water, Pope & Fry 1997 at 650 / 550 / 450 nm (0.340, 0.0565, 0.00922
//        1/m) for R / G / B. M's aerial perspective is removed at H and applied once for the camera's air path to P.
//   Exact condition (else the sample is a counted fallback, awaiting R's refraction rays, request R-2 like solid glass):
//        the refracted ray stays inside the water until band A (water resting on geometry: pools, basins, puddles). The
//        march leaves the water where its point comes in front of the water layer's front surface or where the layer
//        has no water (the ray would continue through air: an exit refraction on the back surface W does not see), the
//        screen (off-screen light), or passes behind an occluder (band A jumps nearer than the ray by more than the
//        step's depth span: the hit is hidden). Every fallback pixel keeps F x reflection + (1 - F) x the straight view's
//        band A radiance attenuated over the straight path to band A (a visible, counted stand-in).
//   Stage 2 (render A's M join): band A under water is lit as in air today (no surface transmission, absorption or
//        caustics on the light's way down): FEATURES_GAME 1.9 stage 2.
#ifndef UNX_WATER_SURFACE_HLSLI
#define UNX_WATER_SURFACE_HLSLI
#include "Bindless.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"
#include "WaterShading.hlsli"

#define WATER_STAT_SHADED 0u     // samples whose refracted ray met band A inside the water (exact path)
#define WATER_STAT_OFFSCREEN 1u  // fallbacks: the refracted ray left the screen
#define WATER_STAT_EXIT 2u       // fallbacks: the refracted ray left the water before band A
#define WATER_STAT_OCCLUDED 3u   // fallbacks: the hit is hidden behind a nearer band A surface
#define WATER_STAT_STEPS 4u      // fallbacks: the march's step bound (never reached by the bound's proof; counted)
#define WATER_STAT_INSIDE 5u     // samples seen from inside the water (the underwater camera, 1.3 (b): not in stage 1)
#define WATER_STAT_UNLIT 6u      // samples whose sun visibility had no resident page (lit)
#define WATER_STAT_COUNT 7u
#define WATER_MARCH_STEPS 9000u  // one step per pixel of the ray's screen path clipped to the image (<= its diagonal: 8K 8,812)

// The frame's water slot table (raw, 16 B per stream slot): vertices SRV (UNX_NONE: not a W stream), material, 0, 0
// (W's upload ring).
struct WaterSlot { uint vertices, material; };
WaterSlot waterSlot(uint table, uint slot)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    const uint2 v = b.Load2(16 * slot);
    WaterSlot s;
    s.vertices = v.x;
    s.material = v.y;
    return s;
}
struct WaterShadeSrvs
{
    uint source, bandADepth, waterVis, waterDepth;  // band A radiance copy (exposed linear), D32 depth, water layer
    uint slots, giCache, statistics, pad;
    AtmosphereSrvs atm;
    ShadowSrvs shadow;
};

// The pixel ray's hit with triangle `tri` of a stream (32 B vertices: (xyz, 1), (normal, 0), three per triangle): the
// point (world), the interpolated normal and whether the ray meets the triangle's plane in front of the camera.
bool waterTriangleHit(uint vertices, uint tri, float3 origin, float3 dir, out float3 p, out float3 n)
{
    ByteAddressBuffer v = ResourceDescriptorHeap[vertices];
    const float3 a = asfloat(v.Load3(96 * tri)), b = asfloat(v.Load3(96 * tri + 32)), c = asfloat(v.Load3(96 * tri + 64));
    const float3 na = asfloat(v.Load3(96 * tri + 16)), nb = asfloat(v.Load3(96 * tri + 48)), nc = asfloat(v.Load3(96 * tri + 80));
    const float3 e1 = b - a, e2 = c - a, g = cross(e1, e2);
    const float denom = dot(g, dir);
    p = origin;
    n = float3(0, 1, 0);
    if (abs(denom) < 1e-30) return false;
    const float s = dot(g, a - origin) / denom;
    p = origin + s * dir;
    // barycentrics of p (the plane point: outside the triangle only by the pixel centre's offset from the covered area)
    const float gg = dot(g, g);
    const float wb = dot(cross(p - a, e2), g) / gg, wc = dot(cross(e1, p - a), g) / gg;
    const float3 m = na * (1 - wb - wc) + nb * wb + nc * wc;
    n = dot(m, m) > 0 ? normalize(m) : normalize(g);
    return s > 0;
}

// Linear view depth and screen position (pixels) of a world point.
float3 waterProject(float3 w)
{
    const float4 c = mul(g_viewProj, float4(w, 1));
    const float2 ndc = c.xy / c.w;
    return float3((ndc.x * 0.5 + 0.5) * g_viewWidth, (0.5 - ndc.y * 0.5) * g_viewHeight, dot(w - g_cameraPosition, -g_view[2].xyz));
}
float waterBandADepth(Texture2D<float> depth, int2 q) { return g_nearPlane / max(depth[q], 1e-30); }  // reversed-Z infinite

// Band A radiance at a screen point (bilinear over the exposed linear copy) with M's aerial perspective removed: the
// surface's own exposed radiance there (air in-scatter and transmittance of the camera's path to depth z).
float3 waterSurfaceRadiance(WaterShadeSrvs s, float2 pos, float z)
{
    Texture2D<float4> src = ResourceDescriptorHeap[s.source];
    const float3 shaded = src.SampleLevel(g_linearClamp, pos / float2(g_viewWidth, g_viewHeight), 0).rgb;
    if (s.atm.transmittance == UNX_NONE || s.atm.aerial == UNX_NONE) return shaded;
    float3 inscatter = 0, transmittance = 1, E = 0;
    atmosphereAirView(s.atm, pos / float2(g_viewWidth, g_viewHeight), z, inscatter, transmittance, E);
    return max(shaded - inscatter * g_exposure, 0) / max(transmittance, 1e-6);
}

// The refracted ray from P along t, marched in screen space: true with the hit H (world) when it meets band A inside
// the water; status a WATER_STAT_* fallback otherwise. Steps follow the ray's screen path one pixel at a time (depth
// interpolated in 1 / z between the projected ends: exact along the 3D line), from P to where the path leaves the screen.
bool waterMarch(WaterShadeSrvs s, float3 P, float3 t, uint slot, out float3 H, out uint status)
{
    Texture2D<float> bandA = ResourceDescriptorHeap[s.bandADepth];
    Texture2D<uint> wvis = ResourceDescriptorHeap[s.waterVis];
    Texture2D<float> wdepth = ResourceDescriptorHeap[s.waterDepth];
    H = P;
    status = WATER_STAT_OFFSCREEN;
    // The far end: the ray's point where it leaves the view volume (or 1 km on), kept in front of the near plane.
    const float3 fwd = -g_view[2].xyz;
    float reach = 1000.0;
    const float tz = dot(t, fwd), pz = dot(P - g_cameraPosition, fwd);
    if (tz < 0) reach = min(reach, max((pz - 2 * g_nearPlane) / -tz, 0.0));
    const float3 a = waterProject(P), bFar = waterProject(P + t * reach);
    // Clip the screen path to the image (the part past it is off-screen light): fraction fmax of the full path.
    const float2 dFull = bFar.xy - a.xy, size = float2(g_viewWidth, g_viewHeight);
    float fmax = 1;
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        if (dFull[axis] > 0) fmax = min(fmax, (size[axis] - a[axis]) / dFull[axis]);
        else if (dFull[axis] < 0) fmax = min(fmax, -a[axis] / dFull[axis]);
    }
    fmax = saturate(fmax);
    const float3 b = float3(a.xy + dFull * fmax, 1.0 / lerp(1.0 / a.z, 1.0 / bFar.z, fmax));
    const float2 d = b.xy - a.xy;
    const float len = max(abs(d.x), abs(d.y));
    const uint steps = uint(min(ceil(len), float(WATER_MARCH_STEPS)));
    if (steps == 0) return false;
    reach *= fmax;  // (the ray parameter of b, for a ray parallel to the image plane)
    float prevZ = a.z;
    for (uint i = 1; i <= steps; ++i)
    {
        const float f = float(i) / float(steps);
        // the 3D line's point at screen fraction f: 1/z linear in screen space
        const float invZ = lerp(1.0 / a.z, 1.0 / b.z, f), z = 1.0 / invZ;
        const float2 pos = a.xy + d * f;
        const int2 q = int2(floor(pos));
        if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) { status = WATER_STAT_OFFSCREEN; return false; }
        const float zA = waterBandADepth(bandA, q);
        if (z >= zA)
        {
            // Band A is reached between the previous step and this one; a jump past more than this step's depth span
            // means the ray went behind a nearer surface there.
            if (zA < prevZ - 1e-4 * zA) { status = WATER_STAT_OCCLUDED; return false; }
            // bisection on the screen fraction for the crossing
            float lo = float(i - 1) / float(steps), hi = f;
            [unroll] for (uint k = 0; k < 12; ++k)
            {
                const float m = 0.5 * (lo + hi), zm = 1.0 / lerp(1.0 / a.z, 1.0 / b.z, m);
                const int2 qm = clamp(int2(floor(a.xy + d * m)), 0, int2(g_viewWidth, g_viewHeight) - 1);
                if (zm >= waterBandADepth(bandA, qm)) hi = m; else lo = m;
            }
            const float zh = 1.0 / lerp(1.0 / a.z, 1.0 / b.z, hi);
            // the world point at view depth zh on the ray P + t u: u from the depth along the view axis
            // (a ray parallel to the image plane keeps its depth: the screen fraction is then the ray parameter's)
            H = abs(tz) > 1e-6 ? P + t * ((zh - pz) / tz) : P + t * (hi * reach);
            status = WATER_STAT_SHADED;
            return true;
        }
        // Still inside the water: this stream's water layer must lie in front of the ray point here.
        const uint v = wvis[q];
        if ((v >> 30) != 3u || ((v >> 24) & 0x3Fu) != slot || wdepth[q] > z * (1 + 1e-5)) { status = WATER_STAT_EXIT; return false; }
        prevZ = z;
    }
    status = WATER_STAT_OFFSCREEN;  // the clipped path ended at the image border without meeting band A
    return false;
}

// The radiance (exposed linear, aerial perspective applied) a water sample at `pixel` of stream (slot, tri) sends to
// the camera; stat receives the WATER_STAT_* the sample counts under.
float3 waterSurfaceShade(WaterShadeSrvs s, uint2 pixel, uint slot, uint tri, out uint stat)
{
    const WaterSlot ws = waterSlot(s.slots, slot);
    const float2 centre = float2(pixel) + 0.5;
    float3 D, Dx, Dy;
    mPixelRay(centre, D, Dx, Dy);
    float3 P, n;
    waterTriangleHit(ws.vertices, tri, g_cameraPosition, D, P, n);
    const float3 v = normalize(g_cameraPosition - P);
    const GpuMaterial m = loadMaterial(ws.material);
    const float ior = m.ior > 1.0001 ? m.ior : kWaterIor;
    const float roughness = max(m.roughness, 0.0);
    const float r = min(sqrt(max(roughness * roughness, 1e-4)), 1.0);
    const bool fromAir = dot(n, v) > 0;
    const float3 nv = fromAir ? n : -n;  // the normal on the camera's side
    const float NoV = saturate(dot(nv, v));
    const float F = waterFresnel(NoV, fromAir ? 1.0 / ior : ior);
    stat = fromAir ? WATER_STAT_SHADED : WATER_STAT_INSIDE;

    // Air of the camera's path to P, sun illuminance there.
    const float z = dot(P - g_cameraPosition, -g_view[2].xyz);
    float3 E = g_sunIlluminance * g_sunColor, inscatter = 0, airT = 1;
    if (s.atm.transmittance != UNX_NONE)
    {
        if (s.atm.aerial != UNX_NONE) atmosphereAirView(s.atm, centre / float2(g_viewWidth, g_viewHeight), z, inscatter, airT, E);
        else E = atmosphereSunIlluminance(s.atm, P);
    }
    // Reflection: the sun's disk and the GI cache's mirror lobe, times F.
    float3 reflected = 0;
    float sunVisibility = 1;
    if (s.shadow.pageTable != UNX_NONE)
    {
        bool resident;
        sunVisibility = shadowSunVisibilityAt(s.shadow, P, nv, z * shPixelAngle(D, Dx), resident);
        if (!resident) { sunVisibility = 1; if (s.statistics != UNX_NONE) { RWByteAddressBuffer st = ResourceDescriptorHeap[s.statistics]; st.InterlockedAdd(4 * WATER_STAT_UNLIT, 1); } }
    }
    const float3 l0 = normalize(g_sunDirection);
    if (sunVisibility > 0 && dot(nv, l0) > 0)
        reflected += shSunSpecular(1.0.xxx, r, max(r * r, 1e-4), 1.0.xxx, nv, v, max(NoV, 1e-4), l0, E, shPixelAngle(D, Dx)) * sunVisibility;
    if (s.giCache != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = s.giCache;
        gi.hash = s.giCache;
        gi.pad0 = gi.pad1 = 0;
        reflected += giCacheRadiance(gi, P, nv, waterReflect(v, nv), reflectionLobeHalfAngle(r, NoV));
    }
    // Transmission along the refracted ray.
    const float3 sigmaA = float3(0.340, 0.0565, 0.00922);  // pure water, Pope & Fry 1997 (1/m)
    float3 transmitted = 0;
    float3 t;
    if (fromAir && waterRefract(v, nv, 1.0 / ior, t))
    {
        float3 H;
        uint status;
        if (waterMarch(s, P, t, slot, H, status))
        {
            const float3 h = waterProject(H);
            transmitted = waterAbsorption(sigmaA, distance(P, H)) * waterSurfaceRadiance(s, h.xy, h.z) / (ior * ior);
        }
        else
        {
            // Fallback (counted): the straight view's band A behind P, attenuated over the straight path.
            Texture2D<float> bandA = ResourceDescriptorHeap[s.bandADepth];
            const float zA = waterBandADepth(bandA, int2(pixel));
            const float along = max(zA - z, 0.0) / max(dot(-v, -g_view[2].xyz), 1e-4);
            transmitted = waterAbsorption(sigmaA, along) * waterSurfaceRadiance(s, centre, zA) / (ior * ior);
            stat = status;
        }
    }
    // (1 - F) of the light crossing; the exposure is in the band A values; the reflected terms are absolute radiance.
    const float3 surface = F * reflected * g_exposure + (1 - F) * transmitted;
    return surface * airT + inscatter * g_exposure;
}
#endif
