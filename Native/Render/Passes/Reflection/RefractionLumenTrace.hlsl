// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Refraction rays on the Lumen path (reflection.lumen_only; FrameServices::traceRefractions): RefractionTrace.hlsl's
// job format, media and exits (water's fluid streams, solid glass, total internal reflection, absorption), with the
// scene hit lit from the surface cache (ReflectionLumenHit.hlsli) - no world GI cache.
// These jobs are the renderer's form of the reference's front-layer translucency reflections (the reflection of the
// frontmost water / glass / coverage surface: LumenFrontLayerTranslucency, a job of medium 0xFF) and of its ray-traced
// translucency (RayTracedTranslucency.usf: the refracted path through the medium); what the path ends on is shaded as
// the reflections' rays shade theirs:
//   hair          the path's first fibre in E's density volume on a segment outside the media, where it lies before the
//                 segment's hit (RayTracing/HitHair.hlsli; raytracing.hair): the groom's proxy under the sun and the
//                 volume's light (no local-light sample: the thread's ray count stays what the callers' bands hold);
//   scene colour  where the view sees the last hit (the depth buffer at its pixel within the relative thickness, the
//                 hit's face turned to the camera by more than the normal threshold): the previous frame's colour there
//                 (the reference's SampleSceneColorAtHit; reflection.lumen_refraction_scene_color_at_hit) - a water
//                 surface then mirrors the lighting the view shows, not the cards' texels;
//   else          the surface cache at the hit; a hit without cards: the sun and the frame's indirect light
//                 (ReflectionLumenHit.hlsli; no local-light sample).
// A path whose throughput's largest channel falls under the threshold ends with what it has (the reference's
// r.Lumen.RayTracedTranslucency.PathThroughputThreshold); the result's largest channel is held to the cap in exposed
// units (its MaxRayIntensity; 0: none).
// One thread per job; a job follows its path through at most REFRACT_SEGMENTS surfaces (one closest-hit trace each);
// the shading - with the thread's one shadow ray, the sun's - follows the walk, outside its loop.
// Result per job (8 B): RGBA16F exposed linear radiance, alpha 1 = traced.
// P[0] = { jobs SRV (raw: header 16 B { count, dispatch x, y, z }, then 48 B jobs), results UAV (raw), max jobs, stream
//          table SRV (raw: per triangle stream slot its vertex buffer SRV) }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli; P[1].w = ray length), P[3].w = asuint(exposure ratio of the previous
// colour)
// P[4] = { card frame SRV (UNX_NONE: none), frame, flags (bit 0: hits read the cards' high levels and report what they
//          want - reflection.lumen_hi_res_surface, one job in a feedback tile's worth reports a frame; bit 1: scene
//          colour at visible hits; bit 2: the previous colour's alpha is its frame's depth - the history depth test,
//          ScreenTrace.hlsli) | cos of the normal threshold as snorm8 << 8 | the path throughput threshold as unorm16
//          << 16, asuint(the result's cap, exposed units; 0: none) }
// P[5] = { previous colour SRV, its width | height << 16, asuint(relative depth thickness), depth SRV }
// P[6], P[7] = RtSceneSrvs; P[8..11] = the previous colour's view-projection (rows). b1 = the main view.
#define RT_SHADOW_TRANSMITTANCE  // (the hits' shadow rays take what the Glass they cross leaves of the light: RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"
#include "Passes/Reflection/ReflectionLumenHit.hlsli"
#include "RayTracing/HitHair.hlsli"

#define RT_INSTANCE_STREAM_BASE 0xFFFF00u
#define RT_MASK_FLUID 0x8u
#define REFRACT_SEGMENTS 4u
#define REFRACT_FLAG_HI_RES 1u
#define REFRACT_FLAG_SCENE_COLOUR 2u
#define REFRACT_FLAG_HISTORY_DEPTH 4u

// The fluid surface's interpolated normal (out of the medium) at a stream hit: vertices of 32 B, normals at +16.
float3 refractStreamNormal(uint slot, RtHit hit)
{
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].w];
    ByteAddressBuffer v = ResourceDescriptorHeap[table.Load(slot * 4)];
    const uint base = hit.primitive * 96;
    const float3 w = rtBary(hit.barycentrics);
    return normalize(asfloat(v.Load3(base + 16)) * w.x + asfloat(v.Load3(base + 48)) * w.y + asfloat(v.Load3(base + 80)) * w.z);
}

// Unpolarized Fresnel reflectance of a dielectric interface for cosine cosI (> 0) on the incident side and
// eta = n_incident / n_transmitted; 1 on total internal reflection.
float refractFresnel(float cosI, float eta)
{
    const float sin2T = eta * eta * (1 - cosI * cosI);
    if (sin2T >= 1) return 1;
    const float cosT = sqrt(1 - sin2T);
    const float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// The previous frame's colour where the view sees the hit (ReflectionLumenTrace.hlsl's rule; this library binds no
// G-buffer, so the surface's turn to the camera is the hit's own face, on the ray's side: a hit on the far side of a
// thin wall the view sees from the front takes nothing).
bool refractSceneColour(RtSceneSrvs scene, RtHit hit, float3 origin, float3 direction, uint job, out float3 colour)
{
    colour = 0;
    const uint flags = P[4].z;
    if ((flags & REFRACT_FLAG_SCENE_COLOUR) == 0 || hit.instance == RT_INSTANCE_EMITTER) return false;
    const float2 size = float2(g_viewWidth, g_viewHeight);
    const float3 hitPoint = origin + direction * hit.t;
    const float4 at = sctProject(hitPoint, size);
    if (!(at.w > 0) || any(at.xy < 0) || any(at.xy >= size)) return false;
    Texture2D<float> depth = ResourceDescriptorHeap[P[5].w];
    const float seenDevice = depth.Load(int3(uint2(at.xy), 0));
    if (!(seenDevice > 0)) return false;
    const float seen = linearDepth(seenDevice);
    if (abs(at.w - seen) >= asfloat(P[5].z) * max(seen, 1e-5)) return false;
    const RtSurface s = rtSurface(scene, hit, origin, direction);
    const float3 face = dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal;
    if (dot(face, normalize(g_cameraPosition - hitPoint)) < (float)((int)(flags << 16) >> 24) / 127.0) return false;
    Texture2D<float4> previous = ResourceDescriptorHeap[P[5].x];
    const float4x4 prevViewProj = float4x4(asfloat(P[8]), asfloat(P[9]), asfloat(P[10]), asfloat(P[11]));
    const float noise = giUnit(job * 0x9E3779B9u + P[4].y * 0x85EBCA77u + 0x2545F491u);
    return sctPreviousColour(previous, uint2(P[5].y & 0xFFFFu, P[5].y >> 16), prevViewProj, hitPoint, asfloat(P[3].w), noise, colour,
                             (flags & REFRACT_FLAG_HISTORY_DEPTH) != 0);
}

[shader("raygeneration")]
void RefractionGen()
{
    const uint job = DispatchRaysIndex().x;
    ByteAddressBuffer jobs = ResourceDescriptorHeap[P[0].x];
    if (job >= min(jobs.Load(0), P[0].z)) return;
    const uint at = 16 + job * 48;
    float3 o = asfloat(jobs.Load3(at)), d = normalize(asfloat(jobs.Load3(at + 16)));
    const uint flags = jobs.Load(at + 28);
    const float3 sigmaA = asfloat(jobs.Load3(at + 32));
    const float ior = max(asfloat(jobs.Load(at + 44)), 1.0);
    int bounces = (int)((flags >> 8) & 3u);
    const RtSceneSrvs scene = rtScene();
    const uint hairParams = rtHairParams(scene);
    const float throughputFloor = (P[4].z >> 16) / 65535.0;
    float3 throughput = 1, L = 0;
    bool inside = (flags & 0xFFu) != 0xFFu;  // medium 0xFF: a reflection ray from the layer's surface, outside
    // what the walk ends on: a groom's fibre, or a surface to shade (after the loop: o and d are then the last segment's)
    RtHit last = rtMiss();
    bool shade = false;
    RtHairHit hair;
    hair.t = -1;
    hair.body = hair.material = 0;
    uint hairSeed = 0;
    [loop] for (uint segment = 0; segment < REFRACT_SEGMENTS; ++segment)
    {
        RayDesc r;
        r.Origin = o;
        r.Direction = d;
        r.TMin = 1e-4;
        r.TMax = giRayLength();
        const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER | (inside ? RT_MASK_FLUID : 0u));
        // the grooms on a segment outside the media (a volume walk: no ray)
        if (!inside && hairParams != UNX_NONE)
        {
            hairSeed = rtHairSeed(uint2(job & 0xFFFFu, (job >> 16) + segment * 4096u), P[4].y);
            hair = rtHairFirst(hairParams, o, d, hit.t < 0 ? giRayLength() : hit.t, hairSeed);
            if (hair.t >= 0) break;
        }
        if (hit.t < 0)
        {
            L = throughput * giSkyRadiance(d);
            break;
        }
        if (inside) throughput *= exp(-sigmaA * hit.t);
        const float3 x = o + d * hit.t;
        bool exits = false;
        float3 n = 0;
        if (hit.instance >= RT_INSTANCE_STREAM_BASE && hit.instance < RT_INSTANCE_STREAM_BASE + 64)
        {
            exits = true;
            n = refractStreamNormal(hit.instance - RT_INSTANCE_STREAM_BASE, hit);
        }
        else if (inside && (flags & 0xFFu) == 1u && hit.instance != RT_INSTANCE_EMITTER)
        {
            const RtSurface sg = rtSurface(scene, hit, o, d);
            if (!sg.frontFace && materialClass(loadMaterial(sg.material)) == MATERIAL_GLASS)
            {
                exits = true;
                n = -sg.normal;
            }
        }
        if (exits)
        {
            if (dot(n, d) < 0) n = -n;  // along the travel: out of the medium
            const float F = refractFresnel(dot(d, n), ior);
            if (F >= 1)
            {
                if (--bounces < 0) break;  // total internal reflection: stays inside while the job's bounces last
                d = reflect(d, -n);
                o = x - n * 1e-3;
            }
            else
            {
                throughput *= (1 - F) * ior * ior;  // radiance over n^2 is conserved across the boundary
                d = normalize(refract(d, -n, ior));
                o = x + n * 1e-3;
                inside = false;
            }
            // (a path that carries next to nothing ends: what lies further adds less than the threshold of itself)
            if (max(throughput.r, max(throughput.g, throughput.b)) < throughputFloor) break;
            continue;
        }
        last = hit;
        shade = true;
        break;
    }
    if (hair.t >= 0)
        L = throughput * rtHairRadiance(scene, hairParams, hair, o, d, hair.t * 2e-3, 1e-3 + 2e-4 * distance(o, g_cameraPosition), hairSeed, true, false);
    else if (shade)
    {
        float3 colour;
        if (refractSceneColour(scene, last, o, d, job, colour)) L = throughput * colour;
        else
            L = throughput * rlShadeHit(scene, last, o, d, 0, 2e-3, P[4].x, UNX_NONE, false, (P[4].z & REFRACT_FLAG_HI_RES) != 0, uint2(job, job >> 4)).radiance;
    }
    float3 e = L * g_exposure;
    if (any(isnan(e)) || any(isinf(e))) e = 0;
    const float cap = asfloat(P[4].w), brightest = max(e.r, max(e.g, e.b));
    if (cap > 0 && brightest > cap) e *= cap / brightest;
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    results.Store2(job * 8, uint2(f32tof16(e.r) | (f32tof16(e.g) << 16), f32tof16(e.b) | (f32tof16(1.0) << 16)));
}
