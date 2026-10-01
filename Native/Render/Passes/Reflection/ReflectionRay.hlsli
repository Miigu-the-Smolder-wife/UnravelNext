// Reflection rays of a job and their records (R-internal): the per-job setup and direction sequence shared by the trace
// (DispatchRays: traversal), shade (compute: hit shading) and combine (compute: the job's value) passes, which replay the
// same seeded VNDF draws from each ray's saved starting seed; and the layout of the rays buffer.
//
// Root constants, the same in every pass: P[0] = { jobs SRV, results UAV (uint3 per job), mode SRV, probes SRV },
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli), ray length in P[1].w, P[3].w = view.screenProbeMaps SRV,
// P[4] = { depth SRV, gbuffer SRV, GI cache UAV (raw), rays per G sample }, P[5] = { frame | experiment << 24, rays buffer
// UAV (raw), ShadowSrvs buffer (ReflectionShade.hlsli), exact set counts }, P[6], P[7] = RtSceneSrvs. Frame constants
// b1 = main view.
//
// Rays buffer (raw): header { rays allocated (atomic), capacity, shadow rays (atomic), jobs; penumbra hits (atomic), ray
// layers UAV, job layers UAV, hit shading flags (REFL_HIT_*, ReflectionShade.hlsli); GI hit accumulator pool SRV
// (UNX_NONE: none), 0, 0, 0 } (48 B = H), then per ray slot:
//   hit records    uint4 at H + slot x 16: { instance | front face << 31 (REFL_RAY_MISS, REFL_RAY_NONE), geometry, primitive,
//                  t }; after the shade pass a penumbra hit's record holds { geometric normal xyz, filter reach }
//   barycentrics   uint  at H + capacity x 16 + slot x 4: 2 x unorm16 (attributes only; the position comes from t)
//   ray -> job     uint  at H + capacity x 20 + slot x 4: job | ray index << 28
//   shaded value   uint4 at H + capacity x 24 + slot x 16: { radiance rg, radiance b | hit distance, sun term rg, sun
//                  term b | valid << 16 } (fp16, radiance and sun term x REFL_STORE_SCALE)
//   sun queue      uint4 at H + capacity x 40 + index x 16: shadow rays from index 0 up { origin xyz, slot }, penumbra
//                  hits from index capacity - 1 down { hit point xyz, slot | filter level << 24 } (ReflectionShadeRays; a
//                  slot queues at most one of the two, so both fit)
//   VNDF seed      uint at H + capacity x 56 + slot x 4: before this ray's first attempt (60 B per slot in all)
// A job whose rays do not fit (header capacity) is traced and shaded inline by the trace pass (ReflectionHit.hlsli) and
// its result written there; results[job] = { first slot, REFL_JOB_SPLIT } marks the split jobs for the combine pass.
#ifndef UNX_REFLECTION_RAY_HLSLI
#define UNX_REFLECTION_RAY_HLSLI
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/Reflection/ReflectionValue.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"  // giRandom, giUnit (the includer defines SKY)

#define REFL_SAMPLE_ATTEMPTS 8u
#define REFL_RAY_MISS 0xFFFFFFFFu
#define REFL_RAY_NONE 0xFFFFFFFEu  // no unmasked direction was drawn
#define REFL_JOB_SPLIT 0xFFFFFFFFu  // results[job].y of a job whose rays are in the rays buffer (never a packed fp16 pair)
#define REFL_JOB_INLINE 0xFFFFFFFEu  // results[job].y of a job left to ReflectionTraceInline (the distance half is >= 0: never)

#define REFL_RAYS_HEADER 48u
uint reflRaysHitOffset(uint slot) { return REFL_RAYS_HEADER + slot * 16; }
uint reflRaysBaryOffset(uint capacity, uint slot) { return REFL_RAYS_HEADER + capacity * 16 + slot * 4; }
uint reflRaysJobOffset(uint capacity, uint slot) { return REFL_RAYS_HEADER + capacity * 20 + slot * 4; }
uint reflRaysValueOffset(uint capacity, uint slot) { return REFL_RAYS_HEADER + capacity * 24 + slot * 16; }
uint reflRaysShadowOffset(uint capacity, uint index) { return REFL_RAYS_HEADER + capacity * 40 + index * 16; }
uint reflRaysSeedOffset(uint capacity, uint slot) { return REFL_RAYS_HEADER + capacity * 56 + slot * 4; }
#define REFL_RAYS_SLOT_BYTES 60u
// Penumbra hit i (0 = the first queued) in the sun queue, from its top.
uint reflRaysPenumbraOffset(uint capacity, uint i) { return reflRaysShadowOffset(capacity, capacity - 1 - i); }
// Reconstruction layers (reflection.layers; ReflectionInternal.hlsli): header words 5 and 6 hold the UAVs of the ray
// layers buffer (REFL_LAYER_RAY_BYTES per slot) and the job layers buffer (REFL_LAYER_JOB_BYTES per job), UNX_NONE when
// the layers are off (ReflectionArgs writes them with the header).
uint reflRayLayersUav(RWByteAddressBuffer rays) { return rays.Load(20); }
uint reflHitFlags(RWByteAddressBuffer rays) { return rays.Load(28); }
uint reflAccPoolSrv(RWByteAddressBuffer rays) { return rays.Load(32); }
uint reflSurfaceCacheUav(RWByteAddressBuffer rays) { return rays.Load(36); }  // the surface cache (SurfaceCache.hlsli), UNX_NONE: hits do not use it
uint reflJobLayersUav(RWByteAddressBuffer rays) { return rays.Load(24); }
// flags: REFL_LAYER_SURFACE (the job has surface hits), REFL_LAYER_NO_DATA (half or more of them found no cache data).
void reflStoreJobLayers(uint uav, uint job, ReflJobLayers l, float3 hitNormal, uint hitInstance, uint flags)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[uav];
    b.Store4(job * REFL_LAYER_JOB_BYTES, uint4(reflLayerPackRadiance(l.stochastic, reflPackOct16(hitNormal)), l.albedo, (hitInstance & 0x00FFFFFFu) | flags));
    b.Store2(job * REFL_LAYER_JOB_BYTES + 16, reflLayerPackRadiance(l.residual, 0));
}

struct ReflJob
{
    uint2 pixel;
    uint mode, rays, seed;
    ReflSurface s;
    float alpha, lobe, coneWidth, coneSpread;
};

// A job's seed: its pixel and the frame (not the job's index: jobs are appended by atomics, their order differs between runs).
uint reflPixelSeed(uint2 pixel) { return giRandom(pixel.x * 7919u + pixel.y * 104729u + (P[5].x & 0xFFFFFFu) * 15485863u); }
uint reflJobSeed(uint job)
{
    StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].x];
    return reflPixelSeed(reflUnpackPixel(jobs[job]));
}

ReflJob reflLoadJob(uint job)
{
    StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[4].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[4].y];
    ReflJob j;
    j.pixel = reflUnpackPixel(jobs[job]);
    j.mode = reflMode(modes.Load(int3(j.pixel, 0)));
    j.s = reflSurface(depth, gbuffer, j.pixel);
    j.alpha = max(j.s.roughness * j.s.roughness, 1e-4);
    j.rays = j.mode == REFL_M ? 1u : (P[4].w & 0xFFFFu);
    j.lobe = reflectionLobeHalfAngle(j.s.roughness, dot(j.s.normal, j.s.view));
    // Ray cone of the pixel (ReflectionShade.hlsli): its width at this surface and its spread after the lobe.
    const float pixelSpread = 2 * g_tanHalfFovY / g_viewHeight;
    j.coneWidth = pixelSpread * distance(j.s.position, g_cameraPosition);
    j.coneSpread = pixelSpread + 2 * tan(j.lobe);
    j.seed = reflPixelSeed(j.pixel);
    return j;
}

// The next direction of the job's sequence. The lobe average is over unmasked directions (the specular directional albedo
// M applies carries the masked loss), so a direction below the surface is redrawn: rejection sampling draws exactly the
// unmasked part of the VNDF distribution. False only when all REFL_SAMPLE_ATTEMPTS draws are masked (extreme grazing).
bool reflNextDirection(ReflJob j, inout uint seed, out float3 dir)
{
    dir = 0;
    [loop] for (uint attempt = 0; attempt < REFL_SAMPLE_ATTEMPTS; ++attempt)
    {
        float2 u = float2(giUnit(seed), giUnit(seed + 1));
        // reflection.lumen_ggx_sampling_bias (P[4].w's high half, unorm16; 0 outside the ray-reuse pipeline): the outer
        // share of the sample disk - the lobe's tail, its rare far-off directions - is not drawn (the reference's
        // GGXSamplingBias: quieter, and the lobe a little narrower than the material's).
        u.x *= 1 - (P[4].w >> 16) / 65535.0;
        seed = giRandom(seed + 2);
        dir = reflSampleGgx(j.s.normal, j.s.view, j.alpha, u);
        if (dot(dir, j.s.normal) > 0) return true;
    }
    return false;
}

// One ray's original rejection sequence, at most eight draws regardless of its
// index. The trace stores the pre-draw seed even when all attempts are masked.
bool reflStoredDirection(ReflJob j, RWByteAddressBuffer rays, uint capacity, uint slot, out float3 dir)
{
    uint seed = rays.Load(reflRaysSeedOffset(capacity, slot));
    return reflNextDirection(j, seed, dir);
}

// The control variate's lobe integral for a G job: the mean of g (the screen-probe cache at texel resolution, the same
// function the rays' g_i sample) over a fixed 4 x 4 stratified VNDF quadrature of the lobe, masked directions excluded as
// the rays exclude them. The control variate is then consistent: E[g_i] = gbar up to the quadrature's error, also where
// the probe maps vary across the lobe (corners, contacts). The prefiltered map at the mirror direction (the K path's
// value) differs from that mean where they vary, and biased the G estimate there. No unmasked direction: the K value.
float3 reflLobeControl(ReflJob j, Texture2D<uint4> probes, GiProbeFootprint footprint, int2 probeCount)
{
    float3 sum = 0;
    uint n = 0;
    [loop] for (uint k = 0; k < 16; ++k)
    {
        const float2 u = (float2(k & 3u, k >> 2) + 0.5) / 4.0;
        const float3 dir = reflSampleGgx(j.s.normal, j.s.view, j.alpha, u);
        if (dot(dir, j.s.normal) <= 0) continue;
        sum += giProbeFootprintRadiance(probes, footprint, probeCount, dir, 0.1763, P[3].w);
        ++n;
    }
    return n > 0 ? sum / n : giProbeFootprintRadiance(probes, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w);
}

float3 reflRayOrigin(ReflSurface s) { return s.position + s.normal * (1e-3 + 2e-4 * s.linearDepth); }

// Seed of the local-light sample at the hit of ray 'ray' of job j (ReflectionLocalShadow traces its visibility,
// ReflectionShadeRays shades it: the same draw): from the job's pixel seed (pixel and frame) and the ray's index, not
// from the job's index - the index is the order the jobs were appended in (atomics), which differs between runs.
uint reflLocalSeed(ReflJob j, uint ray) { return giRandom(j.seed * 7919u + ray * 104729u + 31u); }
// Seed of the sun's shadow ray at that hit (ReflectionShadow, and the inline path's own shadow ray): the same kind of key.
uint reflSunSeed(uint jobSeed, uint ray) { return giRandom(jobSeed * 7919u + ray * 104729u + 17u); }


#endif
