// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.gi.ltv.trace (LumenTranslucencyVolume.hlsli): the cells' rays. DispatchRays over (grid x * 3, grid y * 3, slices of
// the band): thread = the cell's ray texel; ONE ray a thread, bands of at most 262,144 threads. The ray: from the cell's
// sample point (the frame's jitter, kept in front of the depth buffer), direction = its texel of the 3 x 3 equal-area
// sphere map, the point inside the texel jittered per cell column and frame; no face culling (a sample inside a wall
// then sees the wall's back and takes 0 instead of the room behind). It runs to the radiance cache's coverage distance
// where the 8 probes around the sample exist (at the volume's clipmap bias), else to the trace distance.
//   hit    the mesh cards' final lighting at the hit (clReadCards CL_READ_FINAL: the cards' own albedo and emission);
//          a hit without cards: its direct light - the sun by one shadow ray, one local-light sample with its shadow
//          ray (HitLocalSample.hlsli) - through the material's constants, as r.card.radiosity.trace; the back of a
//          one-sided surface: 0. A thread traces at most 3 rays: bands of a third of 262,144 threads;
//   miss   inside the cache's coverage: the cache's radiance in the ray's direction (all 8 probes, weighted); else the sky.
// The radiance is held to P[9].x exposed units (MaxRayIntensity 20) and stored as nits x LTV_SCALE.
// P[0] = { trace UAV (Texture3D R11G11B10F, grid xy * 3), depth SRV, depth pyramid SRV, clipmap bias }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli; P[1].w = the trace distance)
// P[4] = { grid x, grid y, grid z, the band's first slice }
// P[5] = { card frame SRV (UNX_NONE: none), radiance cache params SRV (UNX_NONE: none), indirection SRV, atlas SRV }
// P[6], P[7] = RtSceneSrvs
// P[8] = { asuint(cell jitter xyz), frame }, P[9] = { asuint(ray intensity cap, exposed units; 0: none), the radiance
// cache's depth atlas SRV (UNX_NONE: no probe visibility test), asuint(the depth constraint's threshold, slices) }
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"
#include "Passes/GI/LumenTranslucencyVolumeGrid.hlsli"
#include "RayTracing/HitLocalSample.hlsli"

[shader("raygeneration")]
void LumenTranslucencyVolumeTraceGen()
{
    const uint3 id = DispatchRaysIndex().xyz + uint3(0, 0, P[4].w);
    const uint3 cell = uint3(id.xy / LTV_TRACE_RES, id.z);
    const uint2 texel = id.xy - cell.xy * LTV_TRACE_RES;
    RWTexture3D<float3> trace = ResourceDescriptorHeap[P[0].x];
    if (any(cell >= ltvGridSize())) return;
    trace[id] = 0;
    if (!ltvCellVisible(cell, P[0].z)) return;
    float3 offset = ltvFrameJitter();
    ltvDepthConstraint(cell, offset, P[0].y, asfloat(P[9].z));
    const float3 origin = ltvCellPosition(float3(cell) + offset);
    const uint seed = ltvHash(cell.x + cell.y * 8191u + ltvFrame() * 26699u);
    const float2 uv = (float2(texel) + float2(ltvUnit(seed), ltvUnit(seed ^ 0x9e3779b9u))) / float(LTV_TRACE_RES);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = lrcUvToDirection(uv);
    ray.TMin = 0;
    ray.TMax = giRayLength();
    LrcCoverage coverage = (LrcCoverage)0;
    LrcParams rc = (LrcParams)0;
    float4 cached = 0;
    if (P[5].y != UNX_NONE)
    {
        rc = lrcParams(P[5].y);
        const uint own = lrcClipmap(rc, origin, 0.5);
        if (own < rc.clipmaps)
        {
            coverage.clipmap = min(own + P[0].w, rc.clipmaps - 1);
            coverage.valid = true;
            Texture3D<uint> indirection = ResourceDescriptorHeap[P[5].z];
            const int3 corner = int3(floor(lrcCoordFloat(rc, origin, coverage.clipmap) - 0.5));
            for (uint i = 0; i < 8; ++i)
                if (lrcIndirection(indirection, rc, corner + int3(i & 1, (i >> 1) & 1, i >> 2), coverage.clipmap) >= LRC_USED) coverage.valid = false;
            coverage.minTraceDistance = lrcTMin(rc, coverage.clipmap) + lrcCellSize(rc, coverage.clipmap) * 1.7320508;
        }
        // the cache's answer before the ray; none (probe occlusion): the ray runs its full length
        if (coverage.valid)
        {
            cached = lrcSample(rc, P[5].z, P[5].w, P[9].y, coverage, origin, ray.Direction, lrcSeenFrom(rc, coverage, origin, ray.Direction));
            coverage.valid = cached.a > 0;
        }
        if (coverage.valid) ray.TMax = min(ray.TMax, coverage.minTraceDistance);
    }
    const float dd = dot(ray.Direction, ray.Direction);
    if (!(all(abs(ray.Origin) < 1e9) && dd > 0.98 && dd < 1.02 && ray.TMax > 0 && ray.TMax < 1e30)) return;
    const RtSceneSrvs scene = rtScene();
    float3 radiance = 0;
    const RtHit hit = rtTraceClosest(scene, ray, RAY_FLAG_NONE, RT_MASK_GI);
    if (hit.t < 0)
    {
        if (coverage.valid) radiance = cached.rgb;
        else radiance = giSkyRadiance(ray.Direction);
    }
    else if (hit.instance != RT_INSTANCE_EMITTER && P[5].x != UNX_NONE)
    {
        const RtSurface s = rtSurface(scene, hit, ray.Origin, ray.Direction);
        GpuMaterial m = loadMaterial(s.material);
        if (s.frontFace || (m.classFlags & MATERIAL_TWO_SIDED) != 0)
        {
            const ClSample cards = clReadCards(mcFrame(P[5].x), s.sceneInstance, s.position, s.geometricNormal, CL_READ_FINAL);
            if (cards.valid) radiance = cards.final;
            else
            {
                if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) m.emissive = 0;
                g_rtHitCone = 0.6;  // (a ray of the 3 x 3 sphere map: a cone of about 39 degrees half angle)
                const float bias = 1e-3 + 2e-4 * distance(s.position, g_cameraPosition);
                RtHitLighting L = (RtHitLighting)0;
                const float3 l = normalize(g_sunDirection);
                if (dot(s.normal, l) > 0 || (m.classFlags & 0xFFu) == MATERIAL_FOLIAGE)
                {
                    const float3 e0 = giSunIlluminance(s.position);
                    if (any(e0 > 0))
                    {
                        RayDesc sr;
                        sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * bias;
                        sr.Direction = l;
                        sr.TMin = 0;
                        sr.TMax = giRayLength();
                        L.sunIlluminance = e0;
                        L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
                    }
                }
                L.local = rtHitLocalSample(scene, s, m, -ray.Direction, hit.t * 1.2, bias, seed * 3u + id.z * 7919u + texel.x * 31u + texel.y * 131u);
                radiance = rtHitRadiance(m, s.normal, -ray.Direction, L, 1.2);
            }
        }
    }
    const float cap = asfloat(P[9].x);
    const float brightest = max(radiance.r, max(radiance.g, radiance.b)) * g_exposure;
    if (cap > 0 && brightest > cap) radiance *= cap / brightest;
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;
    trace[id] = max(radiance, 0.0) * LTV_SCALE;
}
