// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.refl.lumen.trace (reflection.lumen_only): the reflection rays of the Lumen path - the structure of Unreal's
// LumenReflectionHardwareRayTracing (ue6-main read as a reference; the code is ours). One thread per job (a traced
// pixel), dispatched indirectly in bands of at most 131,072 jobs; ONE world ray a thread (and, only at a hit that has no
// mesh cards, the sun's shadow ray): at most 262,144 TraceRay calls a dispatch.
//   ray     the pixel's GGX visible-normal direction - the draw the screen trace made for the same pixel and frame
//           (ReflectionReuse.hlsli reuseRay); it starts where the screen trace ended in front of the scene
//           (results[job].x, less the pull-back; 0 without screen traces). A job the screen trace finished is skipped.
//   hair    the ray's first fibre in E's density volume, where it lies before the hit (RayTracing/HitHair.hlsli;
//           raytracing.hair): the groom's proxy, lit by the sun's shadow ray and one local-light sample.
//   miss    the sky in the ray's direction - unless the view shows a surface past the rays' end on the ray's line
//           (distant screen traces, ScreenTrace.hlsli sctDistantTrace; reflection.lumen_distant_screen_traces: the card
//           frame names the stretch, 0 while the rays reach as far as the traces would): the previous frame's colour
//           there.
//   hit     where the view sees the hit point (the depth buffer at its pixel within the relative thickness, the surface
//           there turned to the camera by more than the normal threshold): the previous frame's colour there
//           (SampleSceneColorAtHit) - the lighting the view shows; else the hit's lighting from the surface cache
//           (ReflectionLumenHit.hlsli).
//   water   a hit on a water surface from the air (RayTracing/HitWater.hlsli: W's streams - in the rays' mask when the
//           frame has R's stream table - and meshes of a Water material): F x the hit's ambient light for the mirrored
//           part, the water's own light, and (1 - F) / n^2 x what the refracted ray finds through the water - the
//           thread's second world ray; what it ends on is shaded as a hit without the local-light sample (the thread's
//           three rays: this, that and the sun's). From inside the water (a ray from the bed): the ambient light through
//           the water's extinction - neither side of the surface is followed.
// Result per job: lobe radiance, hit distance, hit motion (reflPackResult) - what the resolve passes read.
// P[0] = { jobs SRV, results UAV (uint3 per job), asuint(exposure ratio of the previous colour), frame (24 bits) | flags
//          << 24 (bit 0: rays start at their screen traces' ends, bit 1: scene colour at visible hits, bit 2: the previous
//          colour's alpha is its frame's depth - the history depth test, ScreenTrace.hlsli, bit 3: hits read the cards'
//          high levels and report what they want - reflection.lumen_hi_res_surface, ReflectionLumenHit.hlsli) }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli; P[1].w = ray length), P[3].w = RayScene's exact set counts UAV (UNX_NONE: none)
// SKY0 (the sun is the atmosphere's: GiSky.hlsli reads no P[3].xyz): P[3].x = R's stream table SRV (HitWater.hlsli;
// UNX_NONE: no streams this frame)
// P[4] = { depth SRV, gbuffer SRV, card frame SRV (UNX_NONE: none), the dispatch's band (bits 0-7) | cos of the normal
//          threshold as snorm8 (bits 8-15) | GGX sampling bias unorm16 << 16 }
// P[5] = { previous colour SRV, its width | height << 16, asuint(relative depth thickness), M's material word SRV (the
//          top layer's roughness, ReflectionInternal.hlsli g_reflWords; UNX_NONE: none) }
// P[6], P[7] = RtSceneSrvs; P[8..11] = the previous colour's view-projection (rows). b1 = the main view.
#define RT_SHADOW_TRANSMITTANCE  // (the hits' shadow rays take what the Glass they cross leaves of the light: RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Reflection/ReflectionReuse.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"
#include "Passes/Reflection/ReflectionLumenHit.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"
#include "RayTracing/HitHair.hlsli"
#include "RayTracing/HitWater.hlsli"

#define RL_BAND 87381u
#define RL_FLAG_SCREEN_START 1u
#define RL_FLAG_SCENE_COLOUR 2u
#define RL_FLAG_HISTORY_DEPTH 4u
#define RL_FLAG_HI_RES 8u

[shader("raygeneration")]
void ReflectionLumenTraceGen()
{
    const uint job = DispatchRaysIndex().x + (P[4].w & 0xFFu) * RL_BAND;
    g_reflWords = P[5].w;
    StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].x];
    const uint entry = jobs[job];
    if (entry & REFL_JOB_DONE) return;  // its value came from the screen trace
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    const uint2 pixel = reflUnpackPixel(entry);
    const uint2 size = uint2(g_viewWidth, g_viewHeight);
    Texture2D<float> depth = ResourceDescriptorHeap[P[4].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[4].y];
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const uint frame = P[0].w & 0xFFFFFFu, flags = P[0].w >> 24;
    const float start = (flags & RL_FLAG_SCREEN_START) != 0 ? asfloat(results[job].x) : 0.0;
    float3 direction;
    float pdf;
    if (!s.valid || !reuseRay(s, pixel, frame, (P[4].w >> 16) / 65535.0, direction, pdf))
    {
        results[job] = reflPackResult(0, 0, 0);
        return;
    }
    const RtSceneSrvs scene = rtScene();
    RayDesc r;
    r.Origin = s.position + s.normal * (1e-3 + 2e-4 * s.linearDepth);
    r.Direction = direction;
    r.TMin = min(max(start, 0.0), giRayLength());
    r.TMax = giRayLength();
#if SKY == SKY_ATMOSPHERE
    const uint streamTable = P[3].x;
#else
    const uint streamTable = UNX_NONE;
#endif
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER | RT_MASK_FAR | (streamTable != UNX_NONE ? RT_MASK_FLUID : 0u));
    // the fog along the ray (FogVolume.hlsli fogOverRay): the surface's place in the view
    const float2 fogUv = (float2(pixel) + 0.5) / float2(size);
    // the ray cone of the pixel after the lobe (the hit's texture footprint)
    const float pixelSpread = 2 * g_tanHalfFovY / g_viewHeight;
    const float coneWidth = pixelSpread * s.linearDepth;
    const float coneSpread = pixelSpread + 2 * tan(reflectionLobeHalfAngle(s.roughness, dot(s.normal, s.view)));
    // the grooms on the ray (HitHair.hlsli): the ray's first fibre from its surface point - the draw the screen trace made
    // for this pixel and frame - where it lies before the hit
    const uint hairParams = rtHairParams(scene);
    if (hairParams != UNX_NONE)
    {
        const uint seed = rtHairSeed(pixel, frame);
        const RtHairHit hair = rtHairFirst(hairParams, s.position, direction, hit.t < 0 ? giRayLength() : hit.t, seed);
        if (hair.t >= 0)
        {
            float3 radiance = rtHairRadiance(scene, hairParams, hair, s.position, direction, coneWidth + hair.t * coneSpread, 1e-3 + 2e-4 * s.linearDepth, seed, true, true);
            if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;
            results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, hair.t, radiance)), hair.t, 0);
            return;
        }
    }
    if (hit.t < 0)
    {
        // distant screen traces: the stretch past the ray's end (needs the previous colour: the scene colour's flag)
        if ((flags & RL_FLAG_SCENE_COLOUR) != 0 && P[4].z != UNX_NONE)
        {
            const LhiRules rules = lhiRules(P[4].z);
            const float4 noise = blueNoise4(pixel, frame);
            float3 far, colour;
            if (rules.distantScreenTrace > 0 &&
                sctDistantTrace(depth, size, r.Origin + direction * giRayLength(), direction, rules.distantScreenTrace, rules.distantSlopeTolerance,
                                noise.w + rules.distantStepOffsetBias, far))
            {
                Texture2D<float4> previous = ResourceDescriptorHeap[P[5].x];
                const float4x4 prevViewProj = float4x4(asfloat(P[8]), asfloat(P[9]), asfloat(P[10]), asfloat(P[11]));
                if (sctPreviousColour(previous, uint2(P[5].y & 0xFFFFu, P[5].y >> 16), prevViewProj, far, asfloat(P[0].z), noise.z, colour, (flags & RL_FLAG_HISTORY_DEPTH) != 0))
                {
                    const float reached = distance(far, r.Origin);
                    results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, reached, colour)), reached, 0);
                    return;
                }
            }
        }
        results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, 65536.0, giSkyRadiance(direction))), giRayLength(), 0);
        return;
    }
    // a water surface (HitWater.hlsli): its own part here, and what the refracted ray finds as the hit to shade
    RtHit shaded = hit;
    float3 shadeOrigin = r.Origin, shadeDirection = direction, through = 1, before = 0;
    float shadeWidth = coneWidth;
    RtWaterSurface water;
    const bool waterHit = rtWaterSurface(scene, hit, r.Origin, direction, streamTable, water);
    if (waterHit)
    {
        const float3 ambient = rtWaterAmbient(water, P[4].z, job * 9781u + frame * 26699u);
        bool ends = true;
        if (water.fromAir)
        {
            float3 into;
            float weight;
            before = rtWaterFromAir(water, direction, ambient, into, weight);
            RayDesc wr;
            wr.Origin = water.position - water.normal * 1e-3;
            wr.Direction = normalize(into);
            wr.TMin = 1e-4;
            wr.TMax = giRayLength();
            // (no streams on this ray: a fluid's far side is not followed - the path's water is then as long as the ray)
            shaded = rtTraceClosest(scene, wr, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER | RT_MASK_FAR);
            if (shaded.t >= 0)
            {
                ends = false;
                through = weight * exp(-water.sigmaT * shaded.t);
                shadeOrigin = wr.Origin;
                shadeDirection = wr.Direction;
                shadeWidth = coneWidth + hit.t * coneSpread;
            }
        }
        else
            before = ambient * exp(-water.sigmaT * hit.t);
        if (ends)
        {
            results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, hit.t, before)), hit.t, 0);
            return;
        }
    }
    // the previous frame's colour where the view sees the hit
    if (!waterHit && (flags & RL_FLAG_SCENE_COLOUR) != 0 && hit.instance != RT_INSTANCE_EMITTER)
    {
        const float3 hitPoint = r.Origin + direction * hit.t;
        const float4 at = sctProject(hitPoint, float2(size));
        if (at.w > 0 && all(at.xy >= 0) && all(at.xy < float2(size)))
        {
            const uint2 hitPixel = uint2(at.xy);
            const float seenDevice = depth.Load(int3(hitPixel, 0));
            if (seenDevice > 0)
            {
                const float seen = linearDepth(seenDevice);
                if (abs(at.w - seen) < asfloat(P[5].z) * max(seen, 1e-5))
                {
                    const ReflSurface t = reflSurface(depth, gbuffer, hitPixel);
                    if (t.valid && dot(t.view, t.normal) >= (float)((int)(P[4].w << 16) >> 24) / 127.0)
                    {
                        Texture2D<float4> previous = ResourceDescriptorHeap[P[5].x];
                        const float4x4 prevViewProj = float4x4(asfloat(P[8]), asfloat(P[9]), asfloat(P[10]), asfloat(P[11]));
                        const float noise = blueNoise4(pixel, frame).z;
                        float3 colour;
                        if (sctPreviousColour(previous, uint2(P[5].y & 0xFFFFu, P[5].y >> 16), prevViewProj, hitPoint, asfloat(P[0].z), noise, colour, (flags & RL_FLAG_HISTORY_DEPTH) != 0))
                        {
                            results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, hit.t, colour)), hit.t, 0);
                            return;
                        }
                    }
                }
            }
        }
    }
    const RlHit shade = rlShadeHit(scene, shaded, shadeOrigin, shadeDirection, shadeWidth, coneSpread, P[4].z, P[3].w, !waterHit, (flags & RL_FLAG_HI_RES) != 0, pixel);
    float3 radiance = before + through * shade.radiance;
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;
    results[job] = reflPackResult(reflStorable(fogOverRay(fogUv, s.linearDepth, r.Origin, direction, hit.t, radiance)), hit.t, waterHit ? 0.0 : shade.motion);
}
