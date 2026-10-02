// unx-kernel: lib_6_6 main
// unx-strict-fp
// Split/overflow and direction replay must agree before FP16 stores and ray traversal.
// unx-variants: SKY=0,1
// Reflection rays (ARCHITECTURE 2.6 G and M paths; DispatchRays with alpha any-hit), one thread per job, dispatched
// indirectly with this frame's job count. The value is the lobe-normalised incident radiance (VNDF samples of the GGX
// lobe, so the normalised integral is the mean of the samples):
//   M job: one sample;
//   G job: reflection.g_rays_per_sample (4) samples with the screen-probe cache as control variate g_i =
//          screenProbeRadiance in direction w_i (texel cone), gbar = its mean over the lobe (reflLobeControl, quadrature):
//          reflLobeEstimate (difference estimator where the rays are as bright as the cache, ratio estimator where darker).
// Traversal only (ARCHITECTURE 2.6 revision 1): the job's rays get slots in the rays buffer and their hit records; the
// hit shading runs in compute (ReflectionShade.hlsl, then ReflectionShadow for off-screen sun visibility, then
// ReflectionCombine for the job's value). A job whose rays do not fit this frame's capacity is marked REFL_JOB_INLINE and
// traced, shaded and combined by ReflectionTraceInline (ReflectionHit.hlsli): the same value, only slower (a separate
// library keeps this one small). Root constants: ReflectionRay.hlsli; GI's cache and screen probes are not bound here
// (UNX_NONE: the pass does not wait for GI's block, ReflectionSystem).
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"

[shader("raygeneration")]
void ReflectionTraceGen()
{
    // (the trace's band is counted in rays: REFL_BAND / the rays of a G job - ReflectionSystem.cpp jobBand)
    const uint job = DispatchRaysIndex().x + reflBand() * (REFL_BAND / max(P[4].w & 0xFFu, 1u));
    StructuredBuffer<uint> jobList = ResourceDescriptorHeap[P[0].x];
    if (jobList[job] & REFL_JOB_DONE) return;  // its value came from the screen trace
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const RtSceneSrvs scene = rtScene();
    const ReflJob j = reflLoadJob(job);
    const float start = (reflHitFlags(rays) & REFL_TRACE_SCREEN_START) != 0 && j.mode == REFL_M ? asfloat(results[job].x) : 0.0;
    const uint capacity = rays.Load(4);
    uint base;
    rays.InterlockedAdd(0, j.rays, base);
    if (base + j.rays > capacity)
    {
        // Slots of this job below the capacity stay allocated: mark them empty so the shade pass skips them (their
        // memory holds another frame's records).
        [loop] for (uint i = 0; i < j.rays && base + i < capacity; ++i) rays.Store4(reflRaysHitOffset(base + i), uint4(REFL_RAY_NONE, 0, 0, 0));
        results[job] = uint3(0, REFL_JOB_INLINE, 0);  // ReflectionTraceInline
        return;
    }
    uint seed = j.seed;
    [loop] for (uint i = 0; i < j.rays; ++i)
    {
        const uint slot = base + i;
        rays.Store(reflRaysJobOffset(capacity, slot), job | (i << 28));
        rays.Store(reflRaysSeedOffset(capacity, slot), seed);
        float3 dir;
        if (!reflNextDirection(j, seed, dir))
        {
            rays.Store4(reflRaysHitOffset(slot), uint4(REFL_RAY_NONE, 0, 0, 0));
            continue;
        }
        RayDesc r;
        r.Origin = reflRayOrigin(j.s);
        r.Direction = dir;
        r.TMin = min(start, giRayLength());
        r.TMax = giRayLength();
        const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER);
        rays.Store4(reflRaysHitOffset(slot), hit.t < 0 ? uint4(REFL_RAY_MISS, 0, 0, 0)
                                                        : uint4(hit.instance | (hit.frontFace << 31), hit.geometry, hit.primitive, asuint(hit.t)));
        if (hit.t >= 0) rays.Store(reflRaysBaryOffset(capacity, slot), reflPackBarycentrics(hit.barycentrics));
    }
    results[job] = uint3(base, REFL_JOB_SPLIT, 0);
}
