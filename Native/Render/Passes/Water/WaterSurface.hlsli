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
#include "WaterFootprint.hlsli"

#define WATER_STAT_SHADED 0u     // samples whose refracted ray met band A inside the water (exact path)
#define WATER_STAT_OFFSCREEN 1u  // fallbacks: the refracted ray left the screen
#define WATER_STAT_EXIT 2u       // fallbacks: the refracted ray left the water before band A
#define WATER_STAT_OCCLUDED 3u   // fallbacks: the hit is hidden behind a nearer band A surface
#define WATER_STAT_STEPS 4u      // fallbacks: the march's step bound (never reached by the bound's proof; counted)
#define WATER_STAT_INSIDE 5u     // samples seen from inside the water (the underwater camera, 1.3 (b): not in stage 1)
#define WATER_STAT_UNLIT 6u      // samples whose sun visibility had no resident page (lit)
#define WATER_STAT_RAY_OVERFLOW 7u    // stage 3: samples whose jobs did not fit the band's list (never by the band sizing)
#define WATER_STAT_REFLECT_JOBS 8u    // stage 3: reflection jobs written (R-W1)
#define WATER_STAT_REFRACT_JOBS 9u    // stage 3: refraction jobs written (R-W2: the fallback samples)
#define WATER_STAT_TRACED 10u         // stage 3: jobs whose result R traced (alpha 1) and the apply pass used
#define WATER_STAT_PLANAR 11u         // calm water: interior samples whose mirror lobe is the planar reflection camera's
#define WATER_STAT_PLANAR_MASK 12u    // words 12..15: pixels WaterPlanarMask gave to candidate plane 0..3 (the CPU's choice)
#define WATER_STAT_COUNT 16u
// Stage 3: a reflection job replaces the GI cache's mirror lobe where the surface's lobe is narrower than the cache's
// resolution (design 2.6: the K path, the cache read, only at half-angles >= 22 degrees; water's 0.02 roughness is ~0.1).
#define WATER_RAY_LOBE_HALF_ANGLE 0.3839724
#define WATER_RAY_TIR_BOUNCES 3u      // total internal reflections a refraction job may take (flags bits 8..9)
#define WATER_MARCH_STEPS 9000u  // one step per pixel of the ray's screen path clipped to the image (<= its diagonal: 8K 8,812)

// Tests: the march's hit screen position (x, y), the step it ended at, and band A's view depth there (WaterInterior's
// debug image).
static float4 g_waterMarchDebug = float4(-1, -1, -1, -1);

// The frame's water slot table (raw, 16 B per stream slot): vertices SRV (UNX_NONE: not a W stream), material, 0, 0
// (W's upload ring).
struct WaterSlot { uint vertices, material; };
// After the 64 slots: the refraction source's box pyramid (WaterFootprint.hlsli): level count, then the SRV of each level
// (level 0 = the source copy itself; WaterSurface.cpp, at most 15 levels).
#define WATER_LEVELS_OFFSET (16u * 64u)
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

// waterTriangleHit plus what the pixel footprint needs (WaterFootprint.hlsli): the ray parameter s (P = origin + s dir),
// the edges, the vertex normals and the unnormalised interpolated normal m.
bool waterTriangleHitFull(uint vertices, uint tri, float3 origin, float3 dir, out float3 p, out float3 n, out float s, out float3 e1, out float3 e2,
                          out float3 na, out float3 nb, out float3 nc, out float3 m)
{
    ByteAddressBuffer v = ResourceDescriptorHeap[vertices];
    const float3 a = asfloat(v.Load3(96 * tri)), b = asfloat(v.Load3(96 * tri + 32)), c = asfloat(v.Load3(96 * tri + 64));
    na = asfloat(v.Load3(96 * tri + 16));
    nb = asfloat(v.Load3(96 * tri + 48));
    nc = asfloat(v.Load3(96 * tri + 80));
    e1 = b - a;
    e2 = c - a;
    const float3 g = cross(e1, e2);
    const float denom = dot(g, dir);
    p = origin;
    n = float3(0, 1, 0);
    m = n;
    s = 0;
    if (abs(denom) < 1e-30) return false;
    s = dot(g, a - origin) / denom;
    p = origin + s * dir;
    const float gg = dot(g, g);
    const float wb = dot(cross(p - a, e2), g) / gg, wc = dot(cross(e1, p - a), g) / gg;
    m = na * (1 - wb - wc) + nb * wb + nc * wc;
    if (!(dot(m, m) > 0)) m = normalize(g);
    n = normalize(m);
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

// Calm water (A14 planar reflection camera; FEATURES_GAME 1.9 stage 3 (i), user's reflection policy: a plane rasterised
// from the mirrored camera is exact where it equals the direct view). A stream with a rest plane (W2 basins) may have a
// reflection camera this frame (WaterSurface.cpp: cost rule); its slot row's word 2 is the candidate index k, and the
// table's planar block holds per k (48 B): the plane (normal towards the camera), the camera's rectangle in main-view
// pixels (x, y, w, h), and the SRVs of its colour (exposed linear radiance), depth and mirror mask (1 = drawn).
// Its pixel q = pixel - (x, y) is the mirror image of the main pixel's ray at the plane point P0: the radiance arriving
// at P0 along the mirror direction r0 of the plane normal - what a reflection job from P0 would return (the solar disk
// excluded as in R's rays: planar views leave it out of the sky, M ShadeSky). The sample at P (normal n, height h off
// the plane) wants the radiance arriving at P along r = reflect(v, n). Image shift of the camera's value against it:
//   normal tilt delta turns the reflected ray by 2 delta; P off the plane moves it parallel by 2 |h| sin(theta_i) <= 2 |h|,
//   a shift of 2 |h| / d at the hit distance d (from the camera's depth: along the main pixel ray, the mirrored hit's view
//   depth equals the main view's, as the camera is main.view x reflect). Used where shift <= WATER_PLANAR_SHIFT px;
//   elsewhere the sample keeps its reflection job (R's ray, exact for any surface).
#define WATER_PLANAR_OFFSET (WATER_LEVELS_OFFSET + 64u)
#define WATER_PLANAR_MAX 4u
#define WATER_PLANAR_SHIFT 0.1  // px: the reflection camera's image offset allowed against the exact mirror ray
float waterPlanarShift(float4 plane, float3 P, float3 n, float d, float pixelAngle)
{
    const float delta = asin(min(length(cross(n, plane.xyz)), 1.0));
    return (2 * delta + 2 * abs(dot(plane.xyz, P) + plane.w) / max(d, 1e-6)) / pixelAngle;
}
// The mask pass's test (before the camera is drawn, the hit distance unknown: the camera's distance to P stands in; the
// shading test below decides with the real one, so this only chooses the pixels the camera draws).
bool waterPlanarCandidate(float4 plane, float3 P, float3 n, float3 v, float pixelAngle)
{
    return dot(n, plane.xyz) > 0 && dot(n, v) > 0 && waterPlanarShift(plane, P, n, distance(g_cameraPosition, P), pixelAngle) <= WATER_PLANAR_SHIFT;
}
// The mirror lobe's radiance from the stream's reflection camera (absolute radiance), or false when it has none or the
// sample is outside its mask or its exactness condition.
bool waterPlanarReflection(uint table, uint slot, uint2 pixel, float3 P, float3 n, float3 D, float pixelAngle, out float3 L)
{
    L = 0;
    ByteAddressBuffer b = ResourceDescriptorHeap[table];
    const uint k = b.Load(16 * slot + 8);
    if (k >= WATER_PLANAR_MAX) return false;
    const uint at = WATER_PLANAR_OFFSET + 48 * k;
    const float4 plane = asfloat(b.Load4(at));
    const uint4 rect = b.Load4(at + 16);
    const uint3 srv = b.Load3(at + 32);
    if (any(pixel < rect.xy) || any(pixel >= rect.xy + rect.zw)) return false;
    const int2 q = int2(pixel - rect.xy);
    Texture2D<uint> mask = ResourceDescriptorHeap[srv.z];
    if (mask[q] != 1u) return false;
    const float3 dir = normalize(D);
    const float toPlane = -(dot(plane.xyz, g_cameraPosition) + plane.w) / dot(plane.xyz, dir);  // the pixel ray to P0
    Texture2D<float> depth = ResourceDescriptorHeap[srv.y];
    const float along = waterBandADepth(depth, q) / max(dot(dir, -g_view[2].xyz), 1e-6);
    if (waterPlanarShift(plane, P, n, along - toPlane, pixelAngle) > WATER_PLANAR_SHIFT) return false;
    Texture2D<float4> colour = ResourceDescriptorHeap[srv.x];
    L = colour.Load(int3(q, 0)).rgb / g_exposure;
    return true;
}

// The refraction source at a screen point and pyramid level (fractional: the two levels around it, bilinear each).
float3 waterSourceAt(WaterShadeSrvs s, float2 pos, float lod)
{
    const float2 uv = pos / float2(g_viewWidth, g_viewHeight);
    ByteAddressBuffer t = ResourceDescriptorHeap[s.slots];
    const uint count = t.Load(WATER_LEVELS_OFFSET);
    if (count <= 1u || !(lod > 0))
    {
        Texture2D<float4> src = ResourceDescriptorHeap[s.source];
        return src.SampleLevel(g_linearClamp, uv, 0).rgb;
    }
    const float l = min(lod, float(count - 1));
    const uint l0 = uint(l), l1 = min(l0 + 1u, count - 1u);
    Texture2D<float4> a = ResourceDescriptorHeap[t.Load(WATER_LEVELS_OFFSET + 4u * (1u + l0))];
    Texture2D<float4> b = ResourceDescriptorHeap[t.Load(WATER_LEVELS_OFFSET + 4u * (1u + l1))];
    return lerp(a.SampleLevel(g_linearClamp, uv, 0).rgb, b.SampleLevel(g_linearClamp, uv, 0).rgb, l - float(l0));
}
// Band A's own exposed radiance at a screen point and level (waterSurfaceRadiance's air removal).
float3 waterSurfaceRadianceLod(WaterShadeSrvs s, float2 pos, float z, float lod)
{
    const float3 shaded = waterSourceAt(s, pos, lod);
    if (s.atm.transmittance == UNX_NONE || s.atm.aerial == UNX_NONE) return shaded;
    float3 inscatter = 0, transmittance = 1, E = 0;
    atmosphereAirView(s.atm, pos / float2(g_viewWidth, g_viewHeight), z, inscatter, transmittance, E);
    return max(shaded - inscatter * g_exposure, 0) / max(transmittance, 1e-6);
}

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

// The four texels of the bilinear quad at screen position pos (texel centres floor(pos - 0.5) and + 1, clamped to the
// image as waterInvDepthAt's block is), gathered at the quad's shared corner: a whole texel from every footprint
// boundary, so the hardware's sub-texel rounding cannot pick another quad.
float4 waterQuad(Texture2D<float> t, float2 pos)
{
    return t.GatherRed(g_linearClamp, (floor(pos - 0.5) + 1) / float2(g_viewWidth, g_viewHeight));
}
float waterMax4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }

// The refracted ray from P along t, marched in screen space: true with the hit H (world) when it meets band A inside
// the water; status a WATER_STAT_* fallback otherwise. Steps follow the ray's screen path one pixel at a time; along it
// the ray's 1 / z is affine (exact for the 3D line) and so is a planar surface's, so where band A is continuous the
// crossing is the exact root of their affine difference between two steps. The crossing belongs to one surface when both
// steps' quads are continuous and band A changes between them by no more than 1.5 times the texel change of that surface
// (plus 1e-3): otherwise the steps straddle an outline, and a surface nearer than the ray at the previous step means the
// ray passed behind it (occluded), a farther one that it meets that surface at this step.
// Inside the water: the water layer (this stream, continuous 1 / z likewise) must lie in front of the ray point.
// Fast step (cost: 2 gathers + 1 load instead of two 3 x 3 blocks + 1 load): waterInvDepthAt's value at pos is the
// bilinear quad's or the nearest texel's (one of the quad's), so it lies within the quad's range. Where the ray is in
// front of the quad's nearest band A texel and behind the quad's farthest water texel (with a margin of several ulps),
// the full step would continue too: the fast step continues and the full values of that step are recomputed only if the
// next step needs them. Every decision, hit and status is the full march's.
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
    bool prevFull = true;  // false: the previous step was fast, its band A values are recomputed when needed
    for (uint i = 1; i <= steps; ++i)
    {
        const float f = float(i) / float(steps);
        const float invRay = lerp(1.0 / a.z, 1.0 / b.z, f), z = 1.0 / invRay;
        const float2 pos = a.xy + d * f;
        if (any(pos < 0) || pos.x >= size.x || pos.y >= size.y) { status = WATER_STAT_OFFSCREEN; return false; }
        const int2 q = clamp(int2(floor(pos)), 0, int2(size) - 1);
        const uint v = wvis[q];
        if (invRay > waterMax4(waterQuad(bandA, pos)) / g_nearPlane * (1 + 1e-6) && (v >> 30) == 3u && ((v >> 24) & 0x3Fu) == slot)
        {
            const float farW = waterMax4(waterQuad(wdepth, pos));
            if (farW < 3.0e38 && (1.0 / farW) * (1 - 1e-6) >= invRay * (1 - 1e-5))
            {
                prevF = f;
                prevZ = z;
                prevFull = false;
                continue;
            }
        }
        if (!prevFull)
        {
            prevInvA = waterInvDepthAt(bandA, a.xy + d * prevF, true, prevContinuous, prevStep);
            prevDiff = lerp(1.0 / a.z, 1.0 / b.z, prevF) - prevInvA;
        }
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
        prevFull = true;
    }
    status = WATER_STAT_OFFSCREEN;  // the clipped path ended at the image border without meeting band A
    return false;
}

// Stage 3 (FEATURES_GAME 1.9; R-W1 / R-W2 through FrameServices::traceRefractions): the terms of a sample that R's rays
// replace. The shaded value keeps stage 1's stand-ins; where a job's result is traced the apply pass
// (WaterRayApply.hlsl) rebuilds the value as base + (traced ? weight x result : fallback) per term, base being the
// value without the replaceable terms (fp32; a difference taken from the stored fp16 value would lose up to 2^-11 of
// the stand-in, which can exceed the traced term many times over):
//   reflection  (medium 0xFF) from P along the mirror direction: F x airT x the radiance arriving at P, in place of the
//               GI cache's mirror lobe (the sun's disk stays the analytic GGX term: R's hit shading and sky exclude it)
//   refraction  (medium 0, the water's streams) from P along the exact Snell direction, for fallback samples only:
//               (1 - F) / n^2 x airT x the radiance arriving inside the water at P (R: absorption, exits, reflections),
//               in place of the straight-view stand-in
struct WaterRayTerms
{
    float3 P, reflectDir, refractDir, sigmaA;
    float ior;
    bool reflect, refract;
    float3 reflectWeight, reflectFallback, refractWeight, refractFallback;
    float3 base;  // the returned value without the two fallback terms
};

// The radiance (exposed linear, aerial perspective applied) a water sample at `pixel` of stream (slot, tri) sends to
// the camera; stat receives the WATER_STAT_* the sample counts under; rays the stage 3 terms.
float3 waterSurfaceShade(WaterShadeSrvs s, uint2 pixel, uint slot, uint tri, out uint stat, out WaterRayTerms rays)
{
    const WaterSlot ws = waterSlot(s.slots, slot);
    const float2 centre = float2(pixel) + 0.5;
    float3 D, Dx, Dy;
    mPixelRay(centre, D, Dx, Dy);
    float3 P, n, e1, e2, na, nb, nc, mRaw;
    float sHit;
    waterTriangleHitFull(ws.vertices, tri, g_cameraPosition, D, P, n, sHit, e1, e2, na, nb, nc, mRaw);
    const float3 v = normalize(g_cameraPosition - P);
    const GpuMaterial m = loadMaterial(ws.material);
    const float ior = m.ior > 1.0001 ? m.ior : kWaterIor;
    const float roughness = max(m.roughness, 0.0);
    const float r = min(sqrt(max(roughness * roughness, 1e-4)), 1.0);
    const bool fromAir = dot(n, v) > 0;
    const float3 nv = fromAir ? n : -n;  // the normal on the camera's side
    const float NoV = saturate(dot(nv, v));
    const float F = waterFresnel(NoV, fromAir ? 1.0 / ior : ior);
    // The pixel footprint (WaterFootprint.hlsli): ray differentials through the interpolated normal. The reflection's
    // direction sweeps `spread` over the pixel: the mirror cone and the sun lobe are integrated over it.
    const WaterDifferentials dif = waterDifferentials(D, Dx, Dy, sHit, e1, e2, na, nb, nc, mRaw, fromAir ? 1.0 : -1.0);
    const float spread = waterReflectionSpread(-v, nv, dif);
    const float alphaPixel = sqrt(max(r * r, 1e-4) * max(r * r, 1e-4) + spread * spread / 6.0);  // GGX alpha^2 + the normals' variance x 2
    const float rPixel = min(sqrt(alphaPixel), 1.0);
    const float lobePixel = max(reflectionLobeHalfAngle(r, NoV), spread);
    stat = fromAir ? WATER_STAT_SHADED : WATER_STAT_INSIDE;
    rays = (WaterRayTerms)0;
    rays.P = P;
    rays.ior = ior;

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
        reflected += shSunSpecular(1.0.xxx, rPixel, alphaPixel, 1.0.xxx, nv, v, max(NoV, 1e-4), l0, E, shPixelAngle(D, Dx)) * sunVisibility;
    // Calm water: the reflection camera's value where the lobe would take a reflection job and the camera is exact here.
    float3 planarL;
    const bool planar = fromAir && lobePixel < WATER_RAY_LOBE_HALF_ANGLE && waterPlanarReflection(s.slots, slot, pixel, P, n, D, shPixelAngle(D, Dx), planarL);
    if (planar)
    {
        reflected += planarL;
        if (s.statistics != UNX_NONE)
        {
            RWByteAddressBuffer st = ResourceDescriptorHeap[s.statistics];
            st.InterlockedAdd(4 * WATER_STAT_PLANAR, 1);
        }
    }
    else if (s.giCache != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = s.giCache;
        gi.hash = s.giCache;
        gi.pad0 = gi.pad1 = 0;
        const float3 mirror = giCacheRadiance(gi, P, nv, waterReflect(v, nv), lobePixel, true);  // + emitter texels (no light loop here)
        reflected += mirror;
        rays.reflectFallback = F * mirror * g_exposure;
    }
    rays.reflect = fromAir && lobePixel < WATER_RAY_LOBE_HALF_ANGLE && !planar;
    rays.reflectDir = waterReflect(v, nv);
    rays.reflectWeight = F;
    // Transmission along the refracted ray.
    // Absorption from the material: baseColor = the medium's transmittance over 1 m (sigma_a = -ln T; authoring rule
    // agreed with engine 2 for W2). Pure water (Pope & Fry 1997, 650 / 550 / 450 nm) is T = (0.712, 0.945, 0.991).
    const float3 sigmaA = -log(clamp(m.baseColor, 1e-6, 1.0));
    rays.sigmaA = sigmaA;
    float3 transmitted = 0;
    float3 t;
    if (fromAir && waterRefract(v, nv, 1.0 / ior, t))
    {
        float3 H;
        uint status;
        if (waterMarch(s, P, t, slot, H, status))
        {
            const float3 h = waterProject(H);
            // The pixel's footprint through the refraction onto band A (the surface's tangent plane at H), integrated:
            // taps along the long side at the level of its width, each with its own path length's absorption.
            const float eta = 1.0 / ior, L = distance(P, H);
            const float3 d = normalize(D);
            const float3 dtx = waterRefractDerivative(d, nv, eta, dif.dDx, dif.dNx), dty = waterRefractDerivative(d, nv, eta, dif.dDy, dif.dNy);
            Texture2D<float> bandADepthTex = ResourceDescriptorHeap[s.bandADepth];
            const float3 nA = waterBandANormal(bandADepthTex, h.xy);
            float dLx, dLy;
            const float3 dHx = waterTransfer(dif.dPx, dtx, t, L, nA, dLx), dHy = waterTransfer(dif.dPy, dty, t, L, nA, dLy);
            const WaterFootprint fp = waterFootprint(waterScreenDerivative(H, dHx), waterScreenDerivative(H, dHy));
            const float dLmajor = fp.majorIsX ? dLx : dLy;
            float3 sum = 0;
            for (uint i = 0; i < fp.count; ++i)
            {
                float along;
                const float2 off = waterFootprintOffset(fp, i, along);
                sum += waterAbsorption(sigmaA, max(L + along * dLmajor, 0.0)) * waterSurfaceRadianceLod(s, h.xy + off, h.z, fp.lod);
            }
            transmitted = sum / float(fp.count) / (ior * ior);
        }
        else
        {
            // Fallback (counted): the straight view's band A behind P, attenuated over the straight path.
            Texture2D<float> bandA = ResourceDescriptorHeap[s.bandADepth];
            const float zA = waterBandADepth(bandA, int2(pixel));
            const float along = max(zA - z, 0.0) / max(dot(-v, -g_view[2].xyz), 1e-4);
            transmitted = waterAbsorption(sigmaA, along) * waterSurfaceRadiance(s, centre, zA) / (ior * ior);
            stat = status;
            rays.refract = true;
            rays.refractDir = t;
            rays.refractWeight = (1 - F) / (ior * ior);
            rays.refractFallback = (1 - F) * transmitted;
        }
    }
    // (1 - F) of the light crossing; the exposure is in the band A values; the reflected terms are absolute radiance.
    const float3 surface = F * reflected * g_exposure + (1 - F) * transmitted;
    // (the camera's air path applies to the replaced terms as to the rest)
    rays.reflectWeight *= airT;
    rays.reflectFallback *= airT;
    rays.refractWeight *= airT;
    rays.refractFallback *= airT;
    const float3 value = surface * airT + inscatter * g_exposure;
    rays.base = value - rays.reflectFallback - rays.refractFallback;
    return value;
}
float3 waterSurfaceShade(WaterShadeSrvs s, uint2 pixel, uint slot, uint tri, out uint stat)
{
    WaterRayTerms rays;
    return waterSurfaceShade(s, pixel, slot, tri, stat, rays);
}

#define WATER_RAY_SAMPLE_BYTES 80u   // a sample record (WaterRayApply.hlsl)
#define WATER_RAY_RECORD 0x80000000u  // a sample's target: a coverage record (entry index) instead of a pixel (x | y << 16)

// Stage 3: appends a sample's jobs and its sample record (WaterRayApply.hlsl, 80 B) to the band's lists, the results zeroed
// (alpha 0 = not traced: the stage 1 value stays). Lists: P[6] = { jobs UAV, results UAV, samples UAV, job capacity },
// P[7].z = sample capacity (UNX_NONE in P[6].x: no lists). The caller sizes the band so both capacities hold every sample.
void waterAppendRays(WaterRayTerms rays, uint target, uint statisticsUav)
{
    if (P[6].x == UNX_NONE || !(rays.reflect || rays.refract)) return;
    RWByteAddressBuffer jobs = ResourceDescriptorHeap[P[6].x];
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[6].y];
    RWByteAddressBuffer samples = ResourceDescriptorHeap[P[6].z];
    const uint n = (rays.reflect ? 1u : 0u) + (rays.refract ? 1u : 0u);
    const uint record = (target & WATER_RAY_RECORD) != 0 ? 1u << 31 : 0u;  // job flags bit 31: a coverage record's
    uint j, k;
    jobs.InterlockedAdd(0, n, j);
    samples.InterlockedAdd(0, 1, k);
    if (j + n > P[6].w || k >= P[7].z)
    {
        if (statisticsUav != UNX_NONE)
        {
            RWByteAddressBuffer statistics = ResourceDescriptorHeap[statisticsUav];
            statistics.InterlockedAdd(4 * WATER_STAT_RAY_OVERFLOW, 1);
        }
        return;
    }
    uint reflectJob = UNX_NONE, refractJob = UNX_NONE;
    if (rays.reflect)
    {
        reflectJob = j++;
        const uint at = 16 + 48 * reflectJob;
        jobs.Store4(at, uint4(asuint(rays.P), reflectJob));
        jobs.Store4(at + 16, uint4(asuint(rays.reflectDir), 0xFFu | record));
        jobs.Store4(at + 32, uint4(0, 0, 0, asuint(1.0)));
        results.Store2(8 * reflectJob, uint2(0, 0));
    }
    if (rays.refract)
    {
        refractJob = j++;
        const uint at = 16 + 48 * refractJob;
        jobs.Store4(at, uint4(asuint(rays.P), refractJob));
        jobs.Store4(at + 16, uint4(asuint(rays.refractDir), (WATER_RAY_TIR_BOUNCES << 8) | record));
        jobs.Store4(at + 32, uint4(asuint(rays.sigmaA), asuint(rays.ior)));
        results.Store2(8 * refractJob, uint2(0, 0));
    }
    const uint at = 16 + WATER_RAY_SAMPLE_BYTES * k;
    samples.Store4(at, uint4(target, reflectJob, refractJob, 0));
    samples.Store4(at + 16, uint4(asuint(rays.reflectWeight), asuint(rays.reflectFallback.x)));
    samples.Store4(at + 32, uint4(asuint(rays.reflectFallback.yz), asuint(rays.refractWeight.xy)));
    samples.Store4(at + 48, uint4(asuint(rays.refractWeight.z), asuint(rays.refractFallback)));
    samples.Store4(at + 64, uint4(asuint(rays.base), 0));
    if (statisticsUav != UNX_NONE)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[statisticsUav];
        if (rays.reflect) statistics.InterlockedAdd(4 * WATER_STAT_REFLECT_JOBS, 1);
        if (rays.refract) statistics.InterlockedAdd(4 * WATER_STAT_REFRACT_JOBS, 1);
    }
}
#endif
