// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// gi.lumen, r.gi.lg.trace: the probes' rays (DispatchRays over the trace atlas: thread = probe atlas coordinate x 8 +
// trace texel). The trace's direction: its texel and level from LgGenerateRays.hlsl (level 1: 8 x 8 map, level 0:
// 16 x 16), the point inside the texel from this frame's ray index of the probe's tile, through the equal-area sphere
// mapping, world space. From the probe's position, lifted off the surface along its normal.
// Hit lighting: with the surface cache (SurfaceCache.hlsli, as the reflection hits use it) the hit marks its cell and,
// where the cell has been lit, takes the cell's irradiance (local lights + multi-bounce) - no light sample, no shadow
// ray, no world-cache read; its own material, emission and sun term stay. Without the cache, or at a cell not lit yet,
// the hit is shaded as the GI cache's own hits are - the sun (one shadow ray into the disk), one local-light sample
// with its shadow ray, and the world cache's irradiance and mirror radiance at the hit (read only). A ray that meets
// an analytic area light's proxy returns 0 (M shades those lights; the proxy still occludes). A miss returns the sky.
// Output: radiance x exposure (RGBA16F, a unused) and the trace word (lgEncodeTrace: distance, hit, moving: the hit
// moves relative to the probe, |probe speed - hit speed| / max(probe depth, 1 m) > P[4].w).
// P[0] = { world cache SRV, ray info SRV (R16_UINT), trace radiance UAV, trace word UAV (R32_UINT) },
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli), ray length; P[3].w = gi.experiment_disable bits (8, 16, 128 as GiTrace),
// P[4] = { sky band (tests), flags, normal bias (float, m), moving threshold (float) }, P[5].x = surface cache UAV
// (0xFFFFFFFF: none - gi.lumen_hit_surface_cache off, surface_cache.enabled off or before its first frame),
// P[5].y / .z / .w = radiance cache params (raw SRV) / indirection SRV / atlas SRV (P[5].y = 0xFFFFFFFF: none -
// lumen.radiance_cache off): where all 8 cache probes around the screen probe exist the ray stops at the cache's
// coverage distance, and a ray that reached it without a hit takes the cache's radiance in its direction (x exposure;
// the sky is in the cache's own misses) - the trace word's bit 31,
// P[6], P[7] = RtSceneSrvs,
// P[8..11] = the common block (P[10].z adaptive SRV, P[10].w / P[11].x / P[11].y probe depth / normal / position SRVs).
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/SurfaceCache/SurfaceCache.hlsli"
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

float lgBias(float3 p) { return 1e-3 + 2e-4 * distance(p, g_cameraPosition); }

[shader("raygeneration")]
void LgTraceGen()
{
    const uint2 coord = DispatchRaysIndex().xy;
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
    // CALL SITE (S2's shared screen trace, as Lumen's LumenScreenTracing: the probes' rays and the reflection rays use one):
    // the ray first walks the depth pyramid; a certain hit takes last frame's lit colour there and skips the ray below (the
    // trace word then carries its distance and hit); otherwise r.TMin = the distance the screen trace cleared.
    // The far field: A's radiance cache (the position is the probe's, as marked by LgRcMark).
    LrcCoverage coverage = (LrcCoverage)0;
    if (P[5].y != 0xFFFFFFFFu)
    {
        coverage = lrcCoverageChecked(lrcParams(P[5].y), P[5].z, positionSpeed.xyz, lgRcDither(atlas));
        if (coverage.valid) r.TMax = min(r.TMax, coverage.minTraceDistance);
    }
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER);
    const uint seed = giRandom(coord.x * 9781u + coord.y * 6271u + lgFrame() * 26699u);

    float3 radiance = 0;
    bool isHit = false, moving = false;
    float distanceToHit = giRayLength();
    bool reachedCache = false;
    if (hit.t < 0 && coverage.valid)
    {
        radiance = lrcSample(lrcParams(P[5].y), P[5].z, P[5].w, coverage, positionSpeed.xyz, r.Direction, giUnit(seed + 31));
        reachedCache = true;
        distanceToHit = r.TMax;
    }
    else if (hit.t < 0)
    {
        radiance = giSkyRadiance(r.Direction);
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
            bool fromSurfaceCache = false;
            if (P[5].x != 0xFFFFFFFFu && (m.classFlags & 0xFFu) != MATERIAL_FOLIAGE)
            {
                RWByteAddressBuffer surfaceCache = ResourceDescriptorHeap[P[5].x];
                const ScLayout layout = scLayout(surfaceCache);
                const float3 face = dot(s.geometricNormal, r.Direction) > 0 ? -s.geometricNormal : s.geometricNormal;
                const float3 bounceAlbedo = saturate(m.baseColor * (1 - m.metallic) + 0.45 * lerp(float3(0.04, 0.04, 0.04), m.baseColor, m.metallic));
                scMark(surfaceCache, layout, s.position, face, bounceAlbedo, m.emissive);
                const ScSample cell = scRead(surfaceCache, layout, s.position, face);
                if (cell.valid)
                {
                    L.irradiance = cell.direct + cell.indirect;
                    L.specularRadiance = L.irradiance / LG_PI;
                    fromSurfaceCache = true;
                }
            }
            // indirect light at the hit: the world cache (read only)
            if (!fromSurfaceCache)
            {
                ByteAddressBuffer cache = ResourceDescriptorHeap[P[0].x];
                const GiHeader h = giHeader(cache);
                const float3 mirror = reflect(r.Direction, s.normal);
                giCacheLightingAt(cache, h, s.position, s.normal, mirror, giLevelForSize(h, footprint), L.irradiance, L.specularRadiance);
            }
            const float3 l = normalize(g_sunDirection);
            const float cosSun = dot(s.normal, l);
            if ((cosSun > 0 || (m.classFlags & 0xFFu) == MATERIAL_FOLIAGE) && (P[3].w & 16) == 0)
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    RayDesc sr;
                    sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * lgBias(s.position);
                    sr.Direction = giSunDirection(seed + 7);
                    sr.TMin = 0;
                    sr.TMax = giRayLength();
                    L.sunIlluminance = e0;
                    L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                }
            }
            if ((P[3].w & 128) == 0 && !fromSurfaceCache)
            {
                const bool oriented = (m.classFlags & 0xFFu) != MATERIAL_FOLIAGE;
                const RtLocalSample ls = rtLocalLightFinish(scene, rtLocalLightChooseOriented(scene, s.position, s.normal, !oriented, giUnit(seed + 11)), s.position,
                                                            giUnit(seed + 12), giUnit(seed + 13), footprint);
                if (ls.valid)
                {
                    // the specular lobe toward the light widened by the ray's cone (the texel holds the cone's mean)
                    GpuMaterial mc = m;
                    const float alpha = modelAlpha(m.roughness);
                    mc.roughness = sqrt(sqrt(alpha * alpha + g_rtHitCone * g_rtHitCone));
                    const float3 f = rtLocalLightBrdfCos(mc, s.normal, -r.Direction, ls.wi, false);
                    if (any(f > 0) && (!ls.castShadow || rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, lgBias(s.position)), RT_MASK_GI)))
                        L.local = f * ls.weight;
                }
            }
            radiance = rtHitRadiance(m, s.normal, -r.Direction, L, footprintPerMetre);
        }
    }
    if (!all(radiance == radiance) || any(radiance < 0)) radiance = 0;
    traceRadiance[coord] = float4(min(radiance * g_exposure, 64000.0), 1);
    traceWord[coord] = lgEncodeTrace(min(distanceToHit, giRayLength()), isHit, moving, reachedCache);
}
