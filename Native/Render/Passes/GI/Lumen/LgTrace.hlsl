// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// gi.lumen, r.gi.lg.trace: the probes' rays (DispatchRays over the trace atlas: thread = probe atlas coordinate x 8 +
// trace texel). The trace's direction: its texel and level from LgGenerateRays.hlsl (level 1: 8 x 8 map, level 0:
// 16 x 16), the point inside the texel from this frame's ray index of the probe's tile, through the equal-area sphere
// mapping, world space. From the probe's position, lifted off the surface along its normal.
// Hit lighting (Unreal's default, r.Lumen.HardwareRayTracing.LightingMode 0: the surface cache): the hit reads the mesh
// cards of its instance (CardLighting.hlsli clReadCards) - the cards' direct light (the sun and the local lights) and
// radiosity - and shades its own material with it; no light sample, no shadow ray, no world-cache read; its own
// emission stays. A hit that has no card there (a deforming instance: skin, wind; a texel the cards do not cover)
// takes the sun (one shadow ray into the disk) and one local-light sample with its shadow ray (HitLocalSample.hlsli), and - only with
// gi.lumen_hit_fallback - the world cache's irradiance; without the fallback its indirect light is 0, as the
// reference's invalid surface-cache sample. A ray that meets an analytic area light's proxy returns 0 (M shades
// those lights; the proxy still occludes). A miss returns the sky.
// E's grooms (RayTracing/HitHair.hlsli; raytracing.hair): the ray's first fibre in the hair density volume, where it lies
// before the hit, is the hit - the groom's proxy, lit by the sun's shadow ray and one local-light sample; the bounce
// light behind it is not seen.
// Output: radiance x exposure (RGBA16F, a unused) and the trace word (lgEncodeTrace: distance, hit, moving: the hit
// moves relative to the probe, |probe speed - hit speed| / max(probe depth, 1 m) > P[4].w).
// P[0] = { world cache SRV, ray info SRV (R16_UINT), trace radiance UAV, trace word UAV (R32_UINT) },
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli), ray length; P[3].w = gi.experiment_disable bits (8, 16, 128 as GiTrace),
// P[4] = { sky band (tests), flags (bit 0: LgScreenTrace ran before - gi.lumen_screen_traces; bit 1: no world-cache
// read at hits - gi.lumen_hit_fallback = false; bits 16..31: the far field's start in metres, 0: none - GiSky.hlsli
// giFarSkyIrradiance), normal bias (float, m),
// moving threshold (float) }, P[5].x = card frame SRV (CardLayout.hlsli mcFrame;
// 0xFFFFFFFF: none - gi.lumen_hit_surface_cache off, surface_cache.mesh_cards off or no card yet),
// P[5].y / .z / .w = radiance cache params (raw SRV) / indirection SRV / atlas SRV (P[5].y = 0xFFFFFFFF: none -
// lumen.radiance_cache off): where all 8 cache probes around the screen probe exist the ray stops at the cache's
// coverage distance, and a ray that reached it without a hit takes the cache's radiance in its direction - all 8 probes,
// trilinear (x exposure; the sky is in the cache's own misses) - and the cache's own hit distance there (P[11].z = the
// cache's depth atlas SRV) - the trace word's bit 31,
// P[6], P[7] = RtSceneSrvs,
// P[8..11] = the common block (P[10].z adaptive SRV, P[10].w / P[11].x / P[11].y probe depth / normal / position SRVs),
// P[11].w = first trace row of this dispatch (the pass splits the atlas into bands of at most gi.lumen_rays_per_dispatch
// rays: each dispatch's work is bounded by its ray count, whatever the resolution).
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"
#include "RayTracing/HitLocalSample.hlsli"
#include "RayTracing/HitHair.hlsli"

float lgBias(float3 p) { return 1e-3 + 2e-4 * distance(p, g_cameraPosition); }
// The world ray after a screen trace starts this much before the point the screen walk reached (m; Unreal's hardware
// ray tracing pull-back bias is 8 cm).
#define LG_SCREEN_PULLBACK 0.08

[shader("raygeneration")]
void LgTraceGen()
{
    // (the atlas is traced in bands of rows, each its own DispatchRays: P[11].w = the band's first row)
    const uint2 coord = DispatchRaysIndex().xy + uint2(0, P[11].w);
    const uint2 atlas = coord / LG_TRACE_RES, texel = coord % LG_TRACE_RES;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    RWTexture2D<float4> traceRadiance = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint> traceWord = ResourceDescriptorHeap[P[0].w];
    if (!(atlas.x < lgProbeViewSize().x && probe < lgProbeCount(adaptive) && probeDepth[atlas] > 0))
    {
        traceRadiance[coord] = 0;
        traceWord[coord] = lgEncodeTrace(0, false, false, false);
        return;
    }
    Texture2D<float2> probeNormal = ResourceDescriptorHeap[P[11].x];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    Texture2D<uint> rayInfo = ResourceDescriptorHeap[P[0].y];
    const float4 positionSpeed = probePosition[atlas];
    const float3 normal = lgDecodeNormal(probeNormal[atlas]);
    const uint2 probePixel = lgProbePixel(adaptive, probe);
    const uint2 tile = lgTileOfPixel(probePixel);

    uint2 rayTexel;
    uint level;
    lgUnpackRay(rayInfo[coord], rayTexel, level);
    const float mapSize = (float)((LG_TRACE_RES * 2) >> level);
    const float2 uv = (float2(rayTexel) + lgTexelCentre(tile)) / mapSize;
    const float coneHalfAngle = acos(1 - 1 / (mapSize * mapSize));
    const float footprintPerMetre = 2 * tan(coneHalfAngle);
    g_rtHitCone = tan(coneHalfAngle);

    const RtSceneSrvs scene = rtScene();
    const float bias = lgBias(positionSpeed.xyz);
    RayDesc r;
    r.Direction = lgSphere(uv);
    r.Origin = positionSpeed.xyz + normal * (asfloat(P[4].z) + bias);
    r.TMin = 0;
    r.TMax = giRayLength();
    // Screen traces first (P[4].y bit 0; LgScreenTrace.hlsl wrote this texel's radiance and word): a screen hit is
    // final - no world ray; otherwise the world ray starts where the screen walk got to, pulled back by
    // LG_SCREEN_PULLBACK (the walk's last step is a pixel wide).
    if ((P[4].y & 1u) != 0)
    {
        const uint screened = traceWord[coord];
        if (lgTraceHit(screened)) return;
        r.TMin = max(lgTraceDistance(screened) - LG_SCREEN_PULLBACK, 0.0);
    }
    // The far field: A's radiance cache (the position is the probe's, as marked by LgRcMark).
    // Its answer for this ray is taken before the ray: where no probe around both sees the ray's start and holds its
    // direction (LumenRadianceCache.hlsli, probe occlusion) the ray runs its full length.
    LrcCoverage coverage = (LrcCoverage)0;
    float4 cached = 0;
    if (P[5].y != 0xFFFFFFFFu)
    {
        const LrcParams rc = lrcParams(P[5].y);
        coverage = lrcCoverageChecked(rc, P[5].z, positionSpeed.xyz, lgRcDither(atlas));
        if (coverage.valid)
        {
            cached = lrcSample(rc, P[5].z, P[5].w, P[11].z, coverage, positionSpeed.xyz, r.Direction, lrcSeenFrom(rc, coverage, r.Origin, r.Direction));
            coverage.valid = cached.a > 0;
        }
        if (coverage.valid) r.TMax = min(r.TMax, coverage.minTraceDistance);
    }
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER);
    const uint seed = giRandom(coord.x * 9781u + coord.y * 6271u + lgFrame() * 26699u);

    float3 radiance = 0;
    bool isHit = false, moving = false;
    uint statClass = 0;  // experiment 2097152: 1 = the hit read no card, 3 = it read its cards
    float distanceToHit = giRayLength();
    bool reachedCache = false;
    // the grooms on the ray: its first fibre from the probe's point (the draw LgScreenTrace made for this texel and frame)
    RtHairHit hair;
    hair.t = -1;
    hair.body = hair.material = 0;
    const uint hairParams = rtHairParams(scene);
    if (hairParams != 0xFFFFFFFFu) hair = rtHairFirst(hairParams, positionSpeed.xyz, r.Direction, hit.t < 0 ? r.TMax : hit.t, rtHairSeed(coord, lgFrame()));
    if (hair.t >= 0)
    {
        isHit = true;
        distanceToHit = hair.t;
        radiance = rtHairRadiance(scene, hairParams, hair, positionSpeed.xyz, r.Direction, hair.t * footprintPerMetre, bias, seed, (P[3].w & 16) == 0, (P[3].w & 128) == 0);
    }
    else if (hit.t < 0 && coverage.valid)
    {
        const LrcParams rc = lrcParams(P[5].y);
        radiance = cached.rgb;
        reachedCache = true;
        distanceToHit = P[11].z != 0xFFFFFFFFu ? lrcSampleDistance(rc, P[5].z, P[11].z, coverage, positionSpeed.xyz, r.Direction) : r.TMax;
    }
    else if (hit.t < 0)
    {
        // (experiment 1048576, diagnostic: a probe ray that leaves the scene takes 0, not the sky - how much of an
        // interior's GI layer is sky light through openings or leaks)
        radiance = (P[3].w & 1048576u) != 0 ? float3(0, 0, 0) : giSkyRadiance(r.Direction);
#if SKY != SKY_ATMOSPHERE
        if (r.Direction.y > asfloat(P[4].x)) radiance = 0;
#endif
    }
    else if (hit.instance == RT_INSTANCE_EMITTER)
    {
        isHit = true;
        distanceToHit = hit.t;
    }
    else
    {
        isHit = true;
        distanceToHit = hit.t;
        GpuInstance inst;
        GpuMesh mesh;
        RtInstance ri;
        RtTriangle tri;
        const RtSurface s = rtSurfaceParts(scene, hit, r.Origin, r.Direction, inst, mesh, ri, tri);
        // the hit's speed: its object-space point through the instance's previous transform (deformed meshes: the
        // instance's motion alone)
        {
            const float3 w = rtBary(hit.barycentrics);
            float3 previous = s.position;
            if ((ri.flags & RT_INSTANCE_DEFORMED) == 0)
            {
                const float3 object = loadVertex(mesh, tri.meshVertex.x).position * w.x + loadVertex(mesh, tri.meshVertex.y).position * w.y +
                                      loadVertex(mesh, tri.meshVertex.z).position * w.z;
                previous = transformPoint(inst.prevObjectToWorld, object);
            }
            const float hitSpeed = distance(s.position, previous);
            moving = abs(positionSpeed.w - hitSpeed) / max(probeDepth[atlas], 1.0) > asfloat(P[4].w);
        }
        GpuMaterial m = loadMaterial(s.material);
        const float footprint = hit.t * footprintPerMetre;
        if ((P[3].w & 8) == 0)
        {
            m = rtHitMaterial(m, s, footprint, dot(s.normal, r.Direction));
            rtHitDecals(scene, s, footprint, m);
        }
        // (the emission of a visible-only emissive is not light for GI: INTERFACES v1.92)
        if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) m.emissive = 0;
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
        if (s.frontFace || twoSided)
        {
            RtHitLighting L = (RtHitLighting)0;
            bool fromCards = false;  // (the cards' direct light holds the sun and the local lights: the hit adds neither)
            if (P[5].x != 0xFFFFFFFFu)
            {
                const float3 face = dot(s.geometricNormal, r.Direction) > 0 ? -s.geometricNormal : s.geometricNormal;
                const ClSample cards = clReadCards(mcFrame(P[5].x), s.sceneInstance, s.position, face, CL_READ_IRRADIANCE);
                if (cards.valid)
                {
                    L.irradiance = cards.direct + cards.indirect;
                    L.specularRadiance = L.irradiance / LG_PI;  // (the lobe at the hit sees the cards' light as uniform)
                    fromCards = true;
                }
                statClass = cards.valid ? 3u : 1u;
            }
            // indirect light at a hit without cards: the world cache, only with gi.lumen_hit_fallback (P[4].y bit 1 clear;
            // read only; experiment 512, attribution: none - one bounce)
            if (!fromCards && (P[4].y & 2u) == 0 && (P[3].w & 512u) == 0)
            {
                ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
                const GiHeader h = giHeader(cache);
                const float3 mirror = reflect(r.Direction, s.normal);
                // gi.bounce_visibility (GI_P1_FLAGS bit 6; as GiTrace's fallback read and the reflection hits'): only cells
                // whose anchor sees the hit count. A footprint-level cell is metres wide for a long ray: the lobby's floor
                // and the sunlit ground outside share one, anchored inside or outside by the order of the first frames -
                // without the rule the probes' hits read daylight or not from run to run (lobby GI layer 22-28 warm or
                // 41-65 grey-blue with the same settings [measured 2026-10-02]).
                g_giStrictVisibility = (cache.Load(GI_P1_FLAGS) & 64u) != 0;
                giCacheLightingAt(cache, h, s.position, s.normal, mirror, giLevelForSize(h, footprint), L.irradiance, L.specularRadiance);
                g_giStrictVisibility = false;
            }
            if (!fromCards) L.irradiance += giFarSkyIrradiance(s.position, s.normal, float(P[4].y >> 16));
            const float3 l = normalize(g_sunDirection);
            const float cosSun = dot(s.normal, l);
            if (!fromCards && (cosSun > 0 || rtHitTransmits(m)) && (P[3].w & 16) == 0)
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    RayDesc sr;
                    sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * lgBias(s.position);
                    // (toward the disk's centre: the same ray every frame - a hit without cards is a deforming surface,
                    // and a random point of the disk would put one-sample noise into the probe)
                    sr.Direction = l;
                    sr.TMin = 0;
                    sr.TMax = giRayLength();
                    L.sunIlluminance = e0;
                    L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                }
            }
            // a hit without cards: one local-light sample (HitLocalSample.hlsli; experiment 128: none, as the reference)
            if (!fromCards && (P[3].w & 128) == 0) L.local = rtHitLocalSample(scene, s, m, -r.Direction, footprint, lgBias(s.position), seed);
            radiance = rtHitRadiance(m, s.normal, -r.Direction, L, footprintPerMetre);
        }
    }
    if (!all(radiance == radiance) || any(radiance < 0)) radiance = 0;
    // Experiment 2097152 (statistics): the hit's surface-cache read class as a colour of exposed value 1 - red: no card
    // read, blue: its cards read; rays without a surface hit (sky, emitters, back faces): 0.
    // The GI layer's channel means then give the cosine-weighted shares.
    if ((P[3].w & 2097152u) != 0)
        radiance = float3(statClass == 1u ? 1.0 : 0.0, statClass == 2u ? 1.0 : 0.0, statClass == 3u ? 1.0 : 0.0) / max(g_exposure, 1e-20);
    traceRadiance[coord] = float4(min(radiance * g_exposure, 64000.0), 1);
    traceWord[coord] = lgEncodeTrace(min(distanceToHit, giRayLength()), isHit, moving, reachedCache);
}
