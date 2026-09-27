// unx-kernel: cs_6_6 main
// unx-variants: SKY=0,1
// Hit shading of this frame's reflection rays in compute (ARCHITECTURE 2.6 revision 1), one thread per ray slot of the
// rays buffer (ReflectionRay.hlsli), dispatched indirectly for the slots allocated. Replays the job's direction, shades
// the hit (ReflectionShade.hlsli: surface, material, GI cache, S's VSM for the sun) or the sky, and stores the value; a
// hit the VSM does not hold keeps its sun term apart and queues one shadow ray (ReflectionShadow); a hit the VSM holds in
// a region that needs the penumbra filter keeps it apart too and queues the filter (ReflectionPenumbra: the same
// estimator in dense waves). Root constants: ReflectionRay.hlsli.
#define REFL_DEFER_PENUMBRA 1
#define GI_CORNER_BATCH 2u  // the cache's corner lookups batched (GiCache.hlsli)
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionShade.hlsli"

uint reflPackHalf2(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint slot = (group.y * 65535u + group.x) * 64u + lane;  // 2D dispatch (ReflectionRayArgs)
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    if (slot >= min(rays.Load(0), capacity)) return;
    const uint4 record = rays.Load4(reflRaysHitOffset(slot));
    const uint valueOffset = reflRaysValueOffset(capacity, slot);
    if (record.x == REFL_RAY_NONE)
    {
        rays.Store4(valueOffset, uint4(0, 0, 0, 0));  // valid bit clear
        return;
    }
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const ReflJob j = reflLoadJob(owner & 0x0FFFFFFFu);
    float3 dir;
    reflRayDirection(j, owner >> 28, dir);
    float3 radiance, sun = 0;
    float distanceToHit;
    float motion = 0;
    if (record.x == REFL_RAY_MISS)
    {
        radiance = giSkyRadiance(dir);
        distanceToHit = 65000;
    }
    else
    {
        RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
        const GiHeader h = giHeader(cache);
        RtHit hit;
        hit.t = asfloat(record.w);
        hit.instance = record.x & 0x00FFFFFFu;  // bit 30: the local-light sample is visible (ReflectionLocalShadow)
        hit.frontFace = record.x >> 31;
        hit.geometry = record.y;
        hit.primitive = record.z;
        hit.barycentrics = reflUnpackBarycentrics(rays.Load(reflRaysBaryOffset(capacity, slot)));
        hit.pad = 0;
        const ReflHitShade o = reflShadeHit(rtSceneSrvs(P[6], P[7]), cache, h, hit, reflRayOrigin(j.s), dir, j.coneWidth, j.coneSpread, reflLocalSeed(owner),
                                            ((record.x >> 30) & 1u) != 0);
        radiance = o.radiance;
        distanceToHit = hit.t;
        motion = o.motion;
        if (o.needsPenumbra)
        {
            // queued from the sun queue's top (one atomic per wave); the hit record, read above, takes the normal and reach
            sun = o.sunTerm;
            const uint lanes = WaveActiveCountBits(true), before = WavePrefixCountBits(true);
            uint first = 0;
            if (WaveIsFirstLane()) rays.InterlockedAdd(16, lanes, first);
            const uint index = WaveReadLaneFirst(first) + before;  // < slots <= capacity (with the shadow rays)
            rays.Store4(reflRaysPenumbraOffset(capacity, index), uint4(asuint(o.shadowOrigin), slot | (o.penumbraLevel << 24)));
            rays.Store4(reflRaysHitOffset(slot), uint4(asuint(o.penumbraNormal), asuint(o.penumbraReach)));
        }
        if (o.needsShadowRay)
        {
            sun = o.sunTerm;
            uint index;
            rays.InterlockedAdd(8, 1u, index);
            rays.Store4(reflRaysShadowOffset(capacity, index), uint4(asuint(o.shadowOrigin), slot));  // index < slots <= capacity
        }
    }
    const float3 r = radiance * REFL_STORE_SCALE, s = sun * REFL_STORE_SCALE;
    rays.Store4(valueOffset, uint4(reflPackHalf2(r.r, r.g), reflPackHalf2(r.b, min(distanceToHit, 65000.0)), reflPackHalf2(s.r, s.g),
                                   f32tof16(s.b) | (1u << 16) | ((f32tof16(min(motion, 60000.0)) & 0x7FFFu) << 17)));  // bit 16 valid, 17.. hit motion (fp16 >= 0)
}
