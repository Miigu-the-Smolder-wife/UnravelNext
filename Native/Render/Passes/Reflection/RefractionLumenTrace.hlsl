// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Refraction rays on the Lumen path (reflection.lumen_only; FrameServices::traceRefractions): RefractionTrace.hlsl's
// job format, media and exits (water's fluid streams, solid glass, total internal reflection, absorption), with the
// scene hit lit from the surface cache (ReflectionLumenHit.hlsli) - no world GI cache, no light sample.
// One thread per job; a job follows its path through at most REFRACT_SEGMENTS surfaces (one closest-hit trace each).
// Result per job (8 B): RGBA16F exposed linear radiance, alpha 1 = traced.
// P[0] = { jobs SRV (raw: header 16 B { count, dispatch x, y, z }, then 48 B jobs), results UAV (raw), max jobs, stream
//          table SRV (raw: per triangle stream slot its vertex buffer SRV) }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli; P[1].w = ray length)
// P[4] = { card frame SRV (UNX_NONE: none), frame, 0, 0 }; P[6], P[7] = RtSceneSrvs. b1 = the main view.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Reflection/ReflectionLumenHit.hlsli"

#define RT_INSTANCE_STREAM_BASE 0xFFFF00u
#define RT_MASK_FLUID 0x8u
#define REFRACT_SEGMENTS 4u

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
    float3 throughput = 1, L = 0;
    bool inside = (flags & 0xFFu) != 0xFFu;  // medium 0xFF: a reflection ray from the layer's surface, outside
    [loop] for (uint segment = 0; segment < REFRACT_SEGMENTS; ++segment)
    {
        RayDesc r;
        r.Origin = o;
        r.Direction = d;
        r.TMin = 1e-4;
        r.TMax = giRayLength();
        const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER | (inside ? RT_MASK_FLUID : 0u));
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
                continue;
            }
            throughput *= (1 - F) * ior * ior;  // radiance over n^2 is conserved across the boundary
            d = normalize(refract(d, -n, ior));
            o = x + n * 1e-3;
            inside = false;
            continue;
        }
        L = throughput * rlShadeHit(scene, hit, o, d, 0, 2e-3, P[4].x, UNX_NONE).radiance;
        break;
    }
    const float3 e = L * g_exposure;
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    results.Store2(job * 8, uint2(f32tof16(e.r) | (f32tof16(e.g) << 16), f32tof16(e.b) | (f32tof16(1.0) << 16)));
}
