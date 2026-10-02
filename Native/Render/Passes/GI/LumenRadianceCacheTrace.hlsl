// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.gi.rc.trace (LumenRadianceCache.hlsli): the probes' rays. DispatchRays over (probe texels, queued traces): thread x =
// the texel of the probe's 32 x 32 equal-area map, y = the trace record. A probe far from the camera, or one forced by
// the budget, is traced at half the resolution (one ray per 2 x 2 texels, the block's value). The ray starts one cell
// diagonal from the probe's centre (lrcTMin: nearer light belongs to the screen probes' own rays) and runs to the trace
// distance. Hit lighting is the screen-probe rays' (Lumen/LgTrace.hlsl): the hit reads the mesh cards of its instance
// (direct light with the sun, radiosity) and shades its own material; a hit without cards takes the sun (one shadow ray
// into the disk), one local-light sample with its shadow ray and - when a world cache is bound - that cache's
// irradiance, else the indirect light the card frame names (LumenHitIndirect.hlsli: while the cache updates, the
// previous frame's translucency volume); the hit's own emission. An analytic area light's proxy returns 0 and
// occludes. A miss returns the sky.
// E's grooms (RayTracing/HitHair.hlsli; raytracing.hair): the ray's first fibre in the hair density volume past the
// ray's start, where it lies before the hit, is the hit (a two-sided one for the probe's occlusion); a groom the ray
// starts inside is left out.
// Probe occlusion (P[4].w != 0; LumenRadianceCache.hlsli): before its ray the thread walks the probe's straight line to
// the ray's start; anything in the way that is not a two-sided sheet and the texel holds nothing (alpha 0, the depth =
// the blocker's distance).
// Output: the trace's radiance (nits x LRC_RADIANCE_SCALE; alpha 1: the texel holds radiance) into the temporary atlas at
// the trace's place, and the hit distance into the probe's place of the depth atlas.
// P[0] = { world cache SRV (UNX_NONE: none), trace records SRV, temporary radiance UAV (RGBA16F), depth atlas UAV (R16_UINT) }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli), P[1].w = the trace distance; P[3].w = gi.experiment_disable bits (8, 16, 128)
// P[4] = { parameters SRV (LrcParams), state SRV, the dispatch's first trace record, probe occlusion on },
// P[5].y = asuint(far-field start, m; 0: none - GiSky.hlsli giFarSkyIrradiance), P[5].x = card frame SRV
// (CardLayout.hlsli mcFrame; UNX_NONE: none). One dispatch holds at most 262,144 rays (LumenRadianceCache.cpp: chunks
// of probes).
// P[6], P[7] = RtSceneSrvs
#define GI_SKY_FOG_RETURN  // (GiSky.hlsli: the sky's share of the sun's light the fog scatters - atmosphere.fog.sun_through_fog)
#define RT_SHADOW_TRANSMITTANCE  // (the hits' shadow rays take what the Glass they cross leaves of the light: RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"
#include "Passes/GI/LumenHitIndirect.hlsli"
#include "RayTracing/HitLocalSample.hlsli"
#include "RayTracing/HitHair.hlsli"
#include "RayTracing/HitFarField.hlsli"

float lrcBias(float3 p) { return 1e-3 + 2e-4 * distance(p, g_cameraPosition); }

[shader("raygeneration")]
void LumenRadianceCacheTraceGen()
{
    const uint2 id = uint2(DispatchRaysIndex().x, DispatchRaysIndex().y + P[4].z);
    const LrcParams p = lrcParams(P[4].x);
    ByteAddressBuffer state = ResourceDescriptorHeap[P[4].y];
    if (id.y >= state.Load(8)) return;
    ByteAddressBuffer traces = ResourceDescriptorHeap[P[0].y];
    const uint4 record = traces.Load4(id.y * 16);
    const float3 centre = asfloat(record.xyz);
    const uint clipmap = (record.w >> 24) & 0x7Fu, slot = record.w & 0xFFFFFFu;
    const uint res = p.probeResolution;
    const uint2 texel = uint2(id.x % res, id.x / res);
    const bool down = (record.w >> 31) != 0 || distance(centre, g_cameraPosition) >= p.downsampleDistance;
    if (down && ((texel.x | texel.y) & 1u) != 0) return;
    const float mapSize = down ? res * 0.5 : res;
    const float2 uv = (float2(texel) + (down ? 1.0 : 0.5)) / res;
    const float coneHalfAngle = acos(1 - 2 / (mapSize * mapSize));  // the texel's solid angle 4 pi / n^2 as a cone
    const float footprintPerMetre = 2 * tan(coneHalfAngle);
    g_rtHitCone = tan(coneHalfAngle);

    const RtSceneSrvs scene = rtScene();
    RayDesc r;
    r.Direction = lrcUvToDirection(uv);
    r.Origin = centre;
    r.TMin = lrcTMin(p, clipmap);
    r.TMax = max(giRayLength(), r.TMin);
    // the probe's straight line to the ray's start
    bool blocked = false;
    uint depthWord = lrcEncodeDepth(0, false, false, false);
    if (P[4].w != 0)
    {
        RayDesc toStart = r;
        toStart.TMin = 0;
        toStart.TMax = r.TMin;
        const RtHit before = rtTraceClosest(scene, toStart, RAY_FLAG_NONE, RT_MASK_GI);
        if (before.t >= 0)
        {
            const RtSurface bs = rtSurface(scene, before, toStart.Origin, toStart.Direction);
            blocked = (loadMaterial(bs.material).classFlags & MATERIAL_TWO_SIDED) == 0;  // (a leaf, a cloth: light passes around it)
            if (blocked) depthWord = lrcEncodeNearDepth(before.t, r.TMin, bs.frontFace);
        }
    }
    RtHit hit = rtMiss();
    if (!blocked) hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER | RT_MASK_FAR);
    const uint seed = giRandom(id.x * 9781u + id.y * 6271u + p.frame * 26699u);

    float3 radiance = 0;
    RtHairHit hair;
    hair.t = -1;
    hair.body = hair.material = 0;
    const uint hairParams = rtHairParams(scene);
    const float3 hairOrigin = r.Origin + r.Direction * r.TMin;
    if (hairParams != 0xFFFFFFFFu && !blocked) hair = rtHairFirst(hairParams, hairOrigin, r.Direction, (hit.t < 0 ? r.TMax : hit.t) - r.TMin, seed, true);
    if (blocked) radiance = 0;
    else if (hair.t >= 0)
    {
        depthWord = lrcEncodeDepth(r.TMin + hair.t, true, true, true);
        radiance = rtHairRadiance(scene, hairParams, hair, hairOrigin, r.Direction, (r.TMin + hair.t) * footprintPerMetre, lrcBias(hairOrigin), seed, (P[3].w & 16) == 0,
                                  (P[3].w & 128) == 0);
    }
    else if (hit.t < 0) radiance = giSkyRadiance(r.Direction);
    else if (hit.instance == RT_INSTANCE_EMITTER) depthWord = lrcEncodeDepth(hit.t, true, true, false);
    else if (hit.instance == RT_INSTANCE_FAR)
    {
        // a proxy of the far field (raytracing.far_field; RayTracing/HitFarField.hlsli)
        depthWord = lrcEncodeDepth(hit.t, true, true, false);
        radiance = rtFarRadiance(scene, hit, r.Origin, r.Direction, (P[3].w & 16) == 0);
    }
    else
    {
        const RtSurface s = rtSurface(scene, hit, r.Origin, r.Direction);
        GpuMaterial m = loadMaterial(s.material);
        const float footprint = hit.t * footprintPerMetre;
        if ((P[3].w & 8) == 0)
        {
            m = rtHitMaterial(m, s, footprint, dot(s.normal, r.Direction));
            rtHitDecals(scene, s, footprint, m);
        }
        if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) m.emissive = 0;  // (not light for GI: INTERFACES v1.92)
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
        depthWord = lrcEncodeDepth(hit.t, true, s.frontFace, twoSided);
        if (s.frontFace || twoSided)
        {
            RtHitLighting L = (RtHitLighting)0;
            bool fromSurfaceCache = false;  // (the cards' direct light holds the sun and the local lights)
            if (P[5].x != UNX_NONE)
            {
                const float3 face = dot(s.geometricNormal, r.Direction) > 0 ? -s.geometricNormal : s.geometricNormal;
                const ClSample cards = clReadCards(mcFrame(P[5].x), s.sceneInstance, s.position, face, CL_READ_IRRADIANCE);
                if (cards.valid)
                {
                    L.irradiance = cards.direct + cards.indirect;
                    L.specularRadiance = L.irradiance / LRC_PI;
                    fromSurfaceCache = true;
                }
            }
            if (!fromSurfaceCache && P[0].x != UNX_NONE)
            {
                ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
                const GiHeader h = giHeader(cache);
                giCacheLightingAt(cache, h, s.position, s.normal, reflect(r.Direction, s.normal), giLevelForSize(h, footprint), L.irradiance, L.specularRadiance);
            }
            bool indirectFound = false;
            if (!fromSurfaceCache && P[0].x == UNX_NONE)
            {
                const float4 e = lhiIrradiance(lhiSources(P[5].x), s.position, s.normal, seed);
                L.irradiance += e.rgb;
                L.specularRadiance += e.rgb / LRC_PI;
                indirectFound = e.a > 0;
            }
            if (!fromSurfaceCache && !indirectFound) L.irradiance += giFarSkyIrradiance(s.position, s.normal, asfloat(P[5].y));
            const float3 l = normalize(g_sunDirection);
            if (!fromSurfaceCache && (dot(s.normal, l) > 0 || rtHitTransmits(m)) && (P[3].w & 16) == 0)
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    RayDesc sr;
                    sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * lrcBias(s.position);
                    sr.Direction = l;  // (the disk's centre: deterministic, as Lumen/LgTrace.hlsl)
                    sr.TMin = 0;
                    sr.TMax = giRayLength();
                    // (the sun through the Glass on the way: what the panes leave of it - RayShaders.hlsli rtShadowTransmittance)
                    const float3 through = rtShadowTransmittance(scene, sr, RT_MASK_HIT_SHADOW | RT_MASK_FAR);
                    L.sunIlluminance = e0 * through;
                    L.sunVisibility = any(through > 0) ? 1.0 : 0.0;
                }
            }
            // a hit without cards: one local-light sample, as Lumen/LgTrace.hlsl (experiment 128: none)
            if (!fromSurfaceCache && (P[3].w & 128) == 0) L.local = rtHitLocalSample(scene, s, m, -r.Direction, footprint, lrcBias(s.position), seed);
            radiance = rtHitRadiance(m, s.normal, -r.Direction, L, footprintPerMetre);
            // a leaf lit from its cards: the other side's light through it (LumenHitIndirect.hlsli)
            if (fromSurfaceCache)
                radiance += lhiFoliageThrough(lhiRules(P[5].x), mcFrame(P[5].x), m, s.sceneInstance, s.position,
                                              dot(s.geometricNormal, r.Direction) > 0 ? -s.geometricNormal : s.geometricNormal);
        }
    }
    if (!all(radiance == radiance) || any(radiance < 0)) radiance = 0;
    const float4 value = blocked ? float4(0, 0, 0, 0) : float4(min(radiance * LRC_RADIANCE_SCALE, 60000.0), 1);
    RWTexture2D<float4> temporary = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint> depthAtlas = ResourceDescriptorHeap[P[0].w];
    const uint2 temporaryBase = uint2(id.y % p.tempProbes, id.y / p.tempProbes) * res, depthBase = lrcAtlasCoord(p, slot) * res;
    const uint block = down ? 2 : 1;
    for (uint by = 0; by < block; ++by)
        for (uint bx = 0; bx < block; ++bx)
        {
            temporary[temporaryBase + texel + uint2(bx, by)] = value;
            depthAtlas[depthBase + texel + uint2(bx, by)] = depthWord;
        }
}
