// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Refraction rays (R-W2 water, R-2 glass; FrameServices::traceRefractions): one ray generation thread per job of the
// caller's list (W's or A's fallback samples). A job starts inside the medium at its surface point with the already
// refracted direction; the ray is traced with absorption e^(-sigma_a t) while inside. It leaves the medium where it meets
// the medium's own surface from inside (medium 0, water: W's fluid triangle streams, RayScene's instances
// RT_INSTANCE_STREAM_BASE + slot, mask RT_MASK_FLUID, normals from the stream's vertices; medium 1, solid glass: a back
// face of a Glass-class scene surface): refracted out with the exit's transmission 1 - F (Fresnel,
// unpolarized), or reflected inside on total internal reflection while the job's bounces last. A hit on the scene is
// shaded as a reflection hit (ReflectionShade.hlsli, the sun by S's VSM or a shadow ray); a miss sees the sky (with the
// cloud layer). Energy the path loses (internal reflection at the exit, bounces exhausted) is counted, not redistributed.
// Medium 0xFF: the job is a reflection ray (R-W1 water, R-1 glass: a layer pixel's mirror direction from its surface
// point, written by the layer's owner where its own lookups are not sharp enough): traced and shaded from outside, no
// absorption, no exit surface.
// Result per job (8 B, P[0].y): RGBA16F exposed linear radiance (x exposure, as band A), alpha 1 = traced: for a medium
// job, the radiance arriving inside the medium at the job's origin along -direction (the caller applies its entry
// (1 - F_in) / n^2); for a reflection job (0xFF), the radiance arriving at the origin along -direction.
// Root constants: ReflectionRay.hlsli's P[1..7]; P[0] = { jobs SRV (raw: header 16 B { count, dispatch x, y, z }, then
// 48 B jobs { float3 origin, uint outputSlot; float3 direction, uint flags (0..7 medium, 8..9 bounces, 31 coverage
// record); float3 sigmaA (1/m), float iorInside }), results UAV (raw), max jobs, stream table SRV (raw: per triangle
// stream slot its vertex buffer SRV) }.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

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
    const float3 n = asfloat(v.Load3(base + 16)) * w.x + asfloat(v.Load3(base + 48)) * w.y + asfloat(v.Load3(base + 80)) * w.z;
    return normalize(n);
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
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    const GiHeader h = giHeader(cache);
    // The job's seed from its own ray (origin, direction) and the frame, not from its index: jobs are appended by atomics
    // (TranslucentComposite, WaterSurface), in an order that differs between runs.
    const uint3 ob = asuint(o), db = asuint(d);
    const uint seed = giRandom(giRandom(ob.x ^ giRandom(ob.y ^ giRandom(ob.z))) ^ giRandom(db.x * 9781u + db.y * 6271u + db.z) ^ ((P[5].x & 0xFFFFFFu) * 6271u + 17u));
    float3 throughput = 1, L = 0;
    bool inside = (flags & 0xFFu) != 0xFFu;  // medium 0xFF: a reflection ray from the layer's surface (R-W1, R-1), outside
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
        // The medium's own surface, from inside: medium 0 (water) = W's fluid streams; medium 1 (solid glass, A's R-2) = a
        // back face of a Glass-class surface (its outward normal is the reverse of the shading normal, which faces the ray).
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
            const float cosI = dot(d, n);
            const float F = refractFresnel(cosI, ior);
            if (F >= 1)
            {
                // Total internal reflection: stays inside while the job's bounces last.
                if (--bounces < 0) break;
                d = reflect(d, -n);
                o = x - n * 1e-3;
                continue;
            }
            // Radiance over n^2 is conserved across the boundary: the air's radiance arrives inside as (1 - F) (n_in/n_out)^2
            // times it (engine 1's closed-form check below30: F Le_floor + (1 - F) n^2 Le_ceiling, +2.2e-4).
            throughput *= (1 - F) * ior * ior;
            d = normalize(refract(d, -n, ior));
            o = x + n * 1e-3;
            inside = false;
            continue;
        }
        const ReflHitShade sh = reflShadeHit(scene, cache, h, hit, o, d, 0, 2e-3, giRandom(seed * 3u + 101u), false);
        L = throughput * (sh.needsShadowRay ? sh.radiance + sh.sunTerm * reflSunVisibility(scene, sh.shadowOrigin, seed) : sh.radiance);
        break;
    }
    const float3 e = L * g_exposure;
    RWByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    results.Store2(job * 8, uint2(f32tof16(e.r) | (f32tof16(e.g) << 16), f32tof16(e.b) | (f32tof16(1.0) << 16)));
}
