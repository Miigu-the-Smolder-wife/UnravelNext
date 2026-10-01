// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.gi.rc.trace (LumenRadianceCache.hlsli): the probes' rays. DispatchRays over (probe texels, queued traces): thread x =
// the texel of the probe's 32 x 32 equal-area map, y = the trace record. A probe far from the camera, or one forced by
// the budget, is traced at half the resolution (one ray per 2 x 2 texels, the block's value). The ray starts one cell
// diagonal from the probe's centre (lrcTMin: nearer light belongs to the screen probes' own rays) and runs to the trace
// distance. Hit lighting is the screen-probe rays' (Lumen/LgTrace.hlsl): with the surface cache the hit marks its cell
// and takes the cell's irradiance where it has been lit; otherwise the world cache's irradiance, the sun (one shadow ray
// into the disk) and one local-light sample with its shadow ray; the hit's own material and emission. An analytic area
// light's proxy returns 0 and occludes. A miss returns the sky.
// Output: the trace's radiance (nits x LRC_RADIANCE_SCALE) into the temporary atlas at the trace's place, and the hit
// distance into the probe's place of the depth atlas.
// P[0] = { world cache SRV (UNX_NONE: none), trace records SRV, temporary radiance UAV (RGBA16F), depth atlas UAV (R16_UINT) }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli), P[1].w = the trace distance; P[3].w = gi.experiment_disable bits (8, 16, 128)
// P[4] = { parameters SRV (LrcParams), state SRV, 0, 0 }, P[5].x = surface cache UAV (UNX_NONE: none)
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/SurfaceCache/SurfaceCache.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"

float lrcBias(float3 p) { return 1e-3 + 2e-4 * distance(p, g_cameraPosition); }

[shader("raygeneration")]
void LumenRadianceCacheTraceGen()
{
    const uint2 id = DispatchRaysIndex().xy;
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
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_EMITTER);
    const uint seed = giRandom(id.x * 9781u + id.y * 6271u + p.frame * 26699u);

    float3 radiance = 0;
    uint depthWord = lrcEncodeDepth(0, false, false, false);
    if (hit.t < 0) radiance = giSkyRadiance(r.Direction);
    else if (hit.instance == RT_INSTANCE_EMITTER) depthWord = lrcEncodeDepth(hit.t, true, true, false);
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
        const bool twoSided = (m.classFlags & MATERIAL_TWO_SIDED) != 0, foliage = (m.classFlags & 0xFFu) == MATERIAL_FOLIAGE;
        depthWord = lrcEncodeDepth(hit.t, true, s.frontFace, twoSided);
        if (s.frontFace || twoSided)
        {
            RtHitLighting L = (RtHitLighting)0;
            bool fromSurfaceCache = false;
            if (P[5].x != UNX_NONE && !foliage)
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
            const float3 l = normalize(g_sunDirection);
            if ((dot(s.normal, l) > 0 || foliage) && (P[3].w & 16) == 0)
            {
                const float3 e0 = giSunIlluminance(s.position);
                if (any(e0 > 0))
                {
                    RayDesc sr;
                    sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * lrcBias(s.position);
                    sr.Direction = giSunDirection(seed + 7);
                    sr.TMin = 0;
                    sr.TMax = giRayLength();
                    L.sunIlluminance = e0;
                    L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                }
            }
            if ((P[3].w & 128) == 0 && !fromSurfaceCache)
            {
                const RtLocalSample ls = rtLocalLightFinish(scene, rtLocalLightChooseOriented(scene, s.position, s.normal, foliage, giUnit(seed + 11)), s.position,
                                                            giUnit(seed + 12), giUnit(seed + 13), footprint);
                if (ls.valid)
                {
                    GpuMaterial mc = m;  // the lobe toward the light widened by the ray's cone (the texel holds the cone's mean)
                    const float alpha = modelAlpha(m.roughness);
                    mc.roughness = sqrt(sqrt(alpha * alpha + g_rtHitCone * g_rtHitCone));
                    const float3 f = rtLocalLightBrdfCos(mc, s.normal, -r.Direction, ls.wi, false);
                    if (any(f > 0) && (!ls.castShadow || rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, lrcBias(s.position)), RT_MASK_GI)))
                        L.local = f * ls.weight;
                }
            }
            radiance = rtHitRadiance(m, s.normal, -r.Direction, L, footprintPerMetre);
        }
    }
    if (!all(radiance == radiance) || any(radiance < 0)) radiance = 0;
    const float4 value = float4(min(radiance * LRC_RADIANCE_SCALE, 60000.0), 1);
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
