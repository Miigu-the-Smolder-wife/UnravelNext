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
//        air, WaterShading.hlsli). sigma_a = -ln(baseColor): the material's baseColor is the transmittance over 1 m
//        (pure water, Pope & Fry 1997 at 650 / 550 / 450 nm: 0.712, 0.945, 0.991). M's aerial perspective is removed at
//        H and applied once for the camera's air path to P.
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

// Tests: the march's hit screen position (x, y), the step it ended at, and band A's view depth there (WaterInterior's
// debug image).
static float4 g_waterMarchDebug = float4(-1, -1, -1, -1);

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

// A depth image's 1 / z at screen position pos, continuous across a surface: over a plane 1 / z is affine in screen
// space, so where the texels around pos are one surface the bilinear value of the four texel centres around pos is exact
// on planes and follows curved surfaces to second order. One surface: all nine texels of the 3 x 3 block around the
// nearest texel present, their second differences along x and y and the bilinear quad's twist |a - b - c + d| each
// within 1e-3 of the nearest texel's value (affine = zero; a step edge, even one aligned with the quad, fails). Across
// a depth discontinuity (an object's outline) or missing samples the value is the nearest texel's. `bandA`: reversed-Z
// device depth (1 / z = d / near); otherwise linear depth (+inf = none, 1 / z = 0). `step`: the largest change of 1 / z
// between neighbouring texels of the block (the march's step test).
float waterInvDepthAt(Texture2D<float> t, float2 pos, bool bandA, out bool continuous, out float step)
{
    const int2 hi = int2(g_viewWidth, g_viewHeight) - 1;
    const int2 n = clamp(int2(floor(pos)), 0, hi);
    float v[3][3];
    float smallest = 3.0e38;
    [unroll] for (int y = 0; y < 3; ++y)
        [unroll] for (int x = 0; x < 3; ++x)
        {
            const float r = t[clamp(n + int2(x - 1, y - 1), 0, hi)];
            v[y][x] = bandA ? r / g_nearPlane : (r < 3.0e38 ? 1.0 / r : 0.0);
            smallest = min(smallest, v[y][x]);
        }
    const float c = v[1][1], tol = 1e-3 * c;
    // the bilinear quad: texel centres i0 .. i0 + 1 around pos, i0 = floor(pos - 0.5), inside the block
    const float2 u = pos - 0.5;
    const int2 o = int2(floor(u)) - n + 1;  // 0 or 1 in each axis
    const float2 f = u - floor(u);
    const float a = v[o.y][o.x], b = v[o.y][o.x + 1], cc = v[o.y + 1][o.x], d = v[o.y + 1][o.x + 1];
    continuous = smallest > 0 && abs(v[1][0] - 2 * c + v[1][2]) <= tol && abs(v[0][1] - 2 * c + v[2][1]) <= tol && abs(a - b - cc + d) <= tol;
    step = max(max(abs(v[1][1] - v[1][0]), abs(v[1][2] - v[1][1])), max(abs(v[1][1] - v[0][1]), abs(v[2][1] - v[1][1])));
    if (continuous) return lerp(lerp(a, b, f.x), lerp(cc, d, f.x), f.y);
    return c;
}

// The refracted ray from P along t, marched in screen space: true with the hit H (world) when it meets band A inside
// the water; status a WATER_STAT_* fallback otherwise. Steps follow the ray's screen path one pixel at a time; along it
// the ray's 1 / z is affine (exact for the 3D line) and so is a planar surface's, so where band A is continuous the
// crossing is the exact root of their affine difference between two steps. The crossing belongs to one surface when both
// steps' quads are continuous and band A changes between them by no more than 1.5 times the texel change of that surface
// (plus 1e-3): otherwise the steps straddle an outline, and a surface nearer than the ray at the previous step means the
// ray passed behind it (occluded), a farther one that it meets that surface at this step.
// Inside the water: the water layer (this stream, continuous 1 / z likewise) must lie in front of the ray point.
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
    bool prevContinuous;
    float prevStep;
    float prevInvA = waterInvDepthAt(bandA, a.xy, true, prevContinuous, prevStep);
    float prevF = 0, prevDiff = 1.0 / a.z - prevInvA, prevZ = a.z;  // (> 0: band A behind P)
    for (uint i = 1; i <= steps; ++i)
    {
        const float f = float(i) / float(steps);
        const float invRay = lerp(1.0 / a.z, 1.0 / b.z, f), z = 1.0 / invRay;
        const float2 pos = a.xy + d * f;
        if (any(pos < 0) || pos.x >= size.x || pos.y >= size.y) { status = WATER_STAT_OFFSCREEN; return false; }
        bool continuous;
        float texelStep;
        const float invA = waterInvDepthAt(bandA, pos, true, continuous, texelStep);
        const float diff = invRay - invA;  // > 0: the ray is in front of band A
        if (diff <= 0)
        {
            const float zA = 1.0 / invA;
            const bool oneSurface = continuous && prevContinuous && abs(invA - prevInvA) <= 1.5 * max(texelStep, prevStep) + 1e-3 * invA;
            if (!oneSurface && zA < prevZ - 1e-4 * zA)
            {
                status = WATER_STAT_OCCLUDED;
                g_waterMarchDebug = float4(pos, float(i), prevZ - zA);
                return false;
            }
            // the affine difference's root between the previous step and this one
            const float fh = oneSurface ? lerp(prevF, f, saturate(prevDiff / max(prevDiff - diff, 1e-30))) : f;
            const float zh = 1.0 / lerp(1.0 / a.z, 1.0 / b.z, fh);
            // the world point at view depth zh on the ray P + t u (a ray parallel to the image plane keeps its depth:
            // the screen fraction is then the ray parameter's)
            H = abs(tz) > 1e-6 ? P + t * ((zh - pz) / tz) : P + t * (fh * reach);
            g_waterMarchDebug = float4(a.xy + d * fh, float(i), zA);
            status = WATER_STAT_SHADED;
            return true;
        }
        // Still inside the water: this stream's water layer must lie in front of the ray point here.
        const int2 q = clamp(int2(floor(pos)), 0, int2(size) - 1);
        const uint v = wvis[q];
        bool waterContinuous;
        float waterStep;
        const float invW = waterInvDepthAt(wdepth, pos, false, waterContinuous, waterStep);
        if ((v >> 30) != 3u || ((v >> 24) & 0x3Fu) != slot || invW <= 0 || invW < invRay * (1 - 1e-5))
        {
            status = WATER_STAT_EXIT;
            g_waterMarchDebug = float4(pos, float(i), invW > 0 ? 1.0 / invW - z : -1);
            return false;
        }
        prevF = f;
        prevDiff = diff;
        prevZ = z;
        prevInvA = invA;
        prevContinuous = continuous;
        prevStep = texelStep;
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
    // Absorption from the material: baseColor = the medium's transmittance over 1 m (sigma_a = -ln T; authoring rule
    // agreed with engine 2 for W2). Pure water (Pope & Fry 1997, 650 / 550 / 450 nm) is T = (0.712, 0.945, 0.991).
    const float3 sigmaA = -log(clamp(m.baseColor, 1e-6, 1.0));
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
