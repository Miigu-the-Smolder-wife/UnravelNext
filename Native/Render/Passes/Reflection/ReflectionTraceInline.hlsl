// unx-kernel: lib_6_6 main
// unx-strict-fp
// Split/overflow and direction replay must agree before FP16 stores and ray traversal.
// unx-variants: SKY=0,1 JOB=1,2 CORNERS=0,1
// The reflection jobs whose rays did not fit this frame's rays buffer (ReflectionTrace marked them REFL_JOB_INLINE):
// traced, shaded and combined in the ray generation shader, one thread per job of the frame (dispatched indirectly with
// the job count; other jobs return at once). JOB = the job mode this library handles (1 REFL_M, 2 REFL_G; a compile
// constant, so each library holds one mode's code and stays under the kernel size limit); both run every frame. The same value as the split passes (ReflectionHit.hlsli), only slower.
// Root constants: ReflectionRay.hlsli.
#define REFL_OVERFLOW 1  // use the same stored attributes/values as the split passes
#define GI_BATCH_CORNERS CORNERS
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

// The inline path of one job (the rays buffer is full): trace, shade and combine in this thread.
void reflTraceInline(ReflJob j, uint job, RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h)
{
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint4> probeTexture = ResourceDescriptorHeap[P[0].w];
    float probeSpacing;
    int2 probeCount;
    const GiProbeFootprint footprint = giProbeFootprint(probeTexture, j.pixel, j.s.normal, j.s.linearDepth, probeSpacing, probeCount);
    uint seed = j.seed;
    float3 sumL = 0, sumG = 0;
    float nearest = 65000;  // the lobe's nearest hit (as ReflectionCombine)
    uint valid = 0;
    float motion = 0;
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint jobLayersUav = reflJobLayersUav(rays);  // reconstruction layers, as ReflectionCombine
    g_reflHitFlags = reflHitFlags(rays);
    g_reflAccPool = reflAccPoolSrv(rays);
    g_reflSurfaceCache = reflSurfaceCacheUav(rays);
    g_reflCardFrame = reflCardFrameSrv(rays);  // (as ReflectionShadeRays: an overflow job's hits read their mesh cards too)
    float3 sumS = 0, sumA = 0;
    uint hits = 0, guide = 0, guideInstance = 0, hitsNoData = 0;
    // One loop over the job's rays and, for a G job, the control variate's lobe points after them (ReflectionRay.hlsli
    // reflLobeControlPoint): the screen-probe lookup stands once in the library. As three inlined copies (a ray's g, the
    // control's sixteen points, its K value) it was 8,200 instructions - 40 KB of a library at the size limit. The sums
    // and their order are ReflectionCombine's: sumL and sumG over the rays, then the control's mean.
    const uint points = j.mode == REFL_G ? j.rays + REFL_LOBE_CONTROL_POINTS : j.rays;
    float3 sumC = 0;
    uint nC = 0;
    [loop] for (uint i = 0; i < points; ++i)
    {
        float3 dir;
        float coneHalfAngle = 0.1763;
        const bool ray = i < j.rays;
        if (ray)
        {
            if (!reflNextDirection(j, seed, dir)) continue;
            RayDesc r;
            r.Origin = reflRayOrigin(j.s);
            r.Direction = dir;
            r.TMin = 0;
            r.TMax = giRayLength();
            float d;
            float hitMotion;
            uint4 l;
            const float3 L = reflHitRadiance(scene, cache, h, r, j.coneWidth, j.coneSpread, reflLocalSeed(j, i), reflSunSeed(j.seed, i), d, hitMotion, l);
            motion = max(motion, hitMotion);
            if (l.w & REFL_LAYER_SURFACE)
            {
                sumS += reflLayerRadiance(l.xy);
                sumA += reflUnpackAlbedo(l.z);
                guide = l.y >> 16;
                guideInstance = l.w & 0x00FFFFFFu;
                ++hits;
                hitsNoData += (l.w >> 30) & 1u;
            }
            sumL += L;
            nearest = min(nearest, d);
            ++valid;
        }
        else if (!reflLobeControlPoint(j, i - j.rays, nC, dir, coneHalfAngle)) continue;
        if (j.mode == REFL_G)
        {
            const float3 g = giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, coneHalfAngle, P[3].w);
            if (ray) sumG += g;
            else
            {
                sumC += g;
                ++nC;
            }
        }
    }
    const float3 gbar = j.mode == REFL_G ? sumC / nC  // (reflLobeControl's mean)
                                         : valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w) : 0;
    const float3 total = reflLobeEstimate(sumL, sumG, valid, gbar);
    results[job] = reflPackResult(total, valid > 0 ? nearest : 0, motion);
    if (jobLayersUav != UNX_NONE)
        reflStoreJobLayers(jobLayersUav, job, reflJobLayers(total, gbar, sumL, sumG, sumS, sumA, valid, hits, j.mode == REFL_G), reflUnpackOct16(guide), guideInstance,
                           (hits > 0 ? REFL_LAYER_SURFACE : 0u) | (hits > 0 && 2 * hitsNoData >= hits ? REFL_LAYER_NO_DATA : 0u));
    // Diagnostics: G samples and those estimated by the ratio branch, one atomic per wave (GI header).
    const uint gSamples = WaveActiveCountBits(j.mode == REFL_G), gRatio = WaveActiveCountBits(j.mode == REFL_G && reflLobeRatio(sumL, sumG, valid, gbar));
    if (WaveIsFirstLane() && gSamples)
    {
        RWByteAddressBuffer statCache = ResourceDescriptorHeap[P[4].z];
        statCache.InterlockedAdd(GI_H_STAT_G_SAMPLES, gSamples);
        if (gRatio) statCache.InterlockedAdd(GI_H_STAT_G_RATIO, gRatio);
    }
    if (j.mode == REFL_G && valid > 0)
    {
        const float lumL = dot(sumL, float3(0.2126, 0.7152, 0.0722)), lumG = dot(sumG, float3(0.2126, 0.7152, 0.0722));
        RWByteAddressBuffer histCache = ResourceDescriptorHeap[P[4].z];
        if (lumG > 1e-8)
            histCache.InterlockedAdd(GI_H_STAT_G_HIST + (uint)clamp(floor(log2(max(lumL, 1e-30) / lumG)) + 4, 0.0, (float)GI_G_HIST_BINS - 1) * 4, 1u);
        else
            histCache.InterlockedAdd(GI_H_STAT_G_ZERO, 1u);
    }
}

[shader("raygeneration")]
void ReflectionTraceInlineGen()
{
    const uint job = DispatchRaysIndex().x + reflBand() * REFL_INLINE_BAND;
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    if (results[job].y != REFL_JOB_INLINE) return;
    ReflJob j = reflLoadJob(job);
    if (j.mode != JOB) return;
    j.mode = JOB;  // the compile constant: the other mode's branches fold away
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    const GiHeader h = giHeader(cache);
    reflTraceInline(j, job, rtScene(), cache, h);
}
