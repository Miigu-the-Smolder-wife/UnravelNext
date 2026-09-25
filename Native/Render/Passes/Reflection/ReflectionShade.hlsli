// Shading of one reflection hit (R-internal), without tracing, so compute passes can run it (r.refl.shade, ARCHITECTURE
// 2.6 revision 1: traversal in DispatchRays, hit shading in compute) as well as the inline path (ReflectionHit.hlsli).
// Needs GiSky.hlsli's root constants (P[1] to P[3]), the GI cache (RW), P[5].x = frame (low 24 bits) |
// reflection.experiment_disable << 24, P[5].z = a raw buffer holding this frame's ShadowSrvs (UNX_NONE: no VSM) and
// P[5].w = RayScene's exact set hit counts UAV (UNX_NONE: none). Experiment bits (cost attribution only, 0 in the shipped
// configuration): 1 = sun shadow rays without any-hit, 2 = no sun visibility (the sun taken as visible), 4 = sun
// visibility by shadow rays only, 8 = hit materials without textures, 16 = no cache lookup at hits, 32 = traversal only
// (a hit returns 1).
//
// The hit's footprint is the ray cone's width there (Akenine-Moller et al., ray cones): 'coneWidth' at the ray origin (the
// primary pixel's width at the reflector, pixel spread x eye distance) plus t x 'coneSpread' (full angle: the pixel's spread
// plus the lobe's 2 tan(half-angle); reflector curvature is not included, so on curved mirrors it is a lower bound). It
// picks the texture level, the cache level and the VSM level at the hit, and filters the sun's specular highlight.
// A reflection hit consumes the cache like a GI hit: its cell (at the footprint's level) is found or created and
// requested for update next frame (hit tier), so cells only reflections see are kept converged.
#ifndef UNX_REFLECTION_SHADE_HLSLI
#define UNX_REFLECTION_SHADE_HLSLI
#include "RayTracing/RayScene.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"

// radiance = the hit's radiance toward the ray origin when the sun term is resolved (no sun, or S's VSM holds the hit);
// otherwise the sun term at full visibility is in sunTerm and needs one shadow ray from shadowOrigin toward a point of
// the solar disk: the value is radiance + sunTerm x visibility.
struct ReflHitShade
{
    float3 radiance, sunTerm, shadowOrigin;
    bool needsShadowRay;
};

ReflHitShade reflShadeHit(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RtHit hit, float3 origin, float3 direction, float coneWidth,
                          float coneSpread)
{
    ReflHitShade o;
    o.radiance = o.sunTerm = o.shadowOrigin = 0;
    o.needsShadowRay = false;
    const uint experiment = P[5].x >> 24;
    if (hit.instance == RT_INSTANCE_EMITTER)
    {
        // An analytic area light (raytracing.emitters, design 12.4 structure 2): its radiance, seen from the reflector.
        o.radiance = rtEmitterRadiance(hit.primitive, origin);
        return o;
    }
    if (experiment & 32)
    {
        o.radiance = 1;  // attribution: traversal only
        return o;
    }
    // Reflection exact set: count this frame's M/G hits on skinned instances (RayScene selects the most-hit ones).
    if (P[5].w != UNX_NONE)
    {
        RtGeometry unused;
        const RtInstance ri = rtResolve(scene, hit, unused);
        const uint deformedIndex = ri.flags >> 8;
        if ((ri.flags & RT_INSTANCE_DEFORMED) != 0 && deformedIndex != 0xFFFFFFu)
        {
            RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[P[5].w];
            InterlockedAdd(counts[deformedIndex], 1u);
        }
    }
    const RtSurface s = rtSurface(scene, hit, origin, direction);
    const float footprint = coneWidth + hit.t * coneSpread;
    GpuMaterial m = loadMaterial(s.material);
    if ((experiment & 8) == 0) m = rtHitMaterial(m, s, footprint, dot(s.normal, direction));
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return o;  // inside closed geometry
    const uint footprintLevel = giLevelForSize(h, footprint);
    RtHitLighting L;
    L.irradiance = L.specularRadiance = 0;
    if ((experiment & 16) == 0)
    {
        if ((experiment & 256) == 0)  // 256: reflection hits read the cache only (the estimator's bias apart from cache feedback)
        {
            bool created;
            const uint e = giFindOrCreate(cache, h, giSurfaceKey(h, s.position, s.normal, footprintLevel), giAnchorAtHit(h, s.position, direction), s.normal, created);
            if (e != GI_ENTRY_PENDING)
            {
                giTouch(cache, h, e);
                giRequestHit(cache, h, e);
            }
        }
        g_giKeepReads = (experiment & 256) == 0;
        float3 sumE, sumL;
        float weight;
        giCacheLevels(cache, h, s.position, s.normal, reflect(direction, s.normal), true, footprintLevel, sumE, sumL, weight);
        L.irradiance = weight > 0 ? sumE / weight : 0;
        L.specularRadiance = weight > 0 ? sumL / weight : 0;
        // Diagnostics: lookups and misses (no updated cell at any level searched), one atomic per wave.
        const uint lookups = WaveActiveCountBits(true), misses = WaveActiveCountBits(weight <= 0);
        if (WaveIsFirstLane())
        {
            cache.InterlockedAdd(GI_H_STAT_HIT_LOOKUPS, lookups);
            if (misses) cache.InterlockedAdd(GI_H_STAT_HIT_MISSES, misses);
        }
    }
    const float3 v = -direction;
    L.sunIlluminance = 0;
    L.sunVisibility = 0;
    const float3 l = normalize(g_sunDirection);
    if (dot(s.normal, l) > 0 || materialClass(m) == MATERIAL_FOLIAGE)
    {
        L.sunIlluminance = giSunIlluminance(s.position);
        if (any(L.sunIlluminance > 0))
        {
            // The direct view's estimator from S's VSM where it holds the hit at the ray cone's footprint (SMRT disk
            // integral: shadows agree with the direct view and the planar camera); a shadow ray elsewhere.
            bool resident = false;
            if (P[5].z != UNX_NONE && (experiment & 6) == 0)
            {
                ByteAddressBuffer b = ResourceDescriptorHeap[P[5].z];
                const uint4 a = b.Load4(0), c = b.Load4(16);
                ShadowSrvs vsm;
                vsm.pageTable = a.x; vsm.pool = a.y; vsm.blocks = a.z; vsm.searchBound = a.w;
                vsm.constants = c.x; vsm.lights = c.y; vsm.pad0 = c.z; vsm.layers = c.w;
                L.sunVisibility = shadowSunVisibilityAt(vsm, s.position, s.geometricNormal, footprint, resident);
            }
            if (experiment & 2)
            {
                L.sunVisibility = 1;
                resident = true;
            }
            if (!resident)
            {
                // Sun term at full visibility apart; offset along the geometric normal on the sun's side (leaves
                // transmit: the ray leaves the lit side; the shading normal can point under the triangle).
                L.sunVisibility = 0;
                const float3 unlit = rtHitRadiance(m, s.normal, v, L, coneSpread);
                L.sunVisibility = 1;
                o.sunTerm = rtHitRadiance(m, s.normal, v, L, coneSpread) - unlit;
                o.radiance = unlit;
                const float side = dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0;
                o.shadowOrigin = s.position + side * s.geometricNormal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
                o.needsShadowRay = true;
                return o;
            }
        }
    }
    o.radiance = rtHitRadiance(m, s.normal, v, L, coneSpread);
    return o;
}

#endif
