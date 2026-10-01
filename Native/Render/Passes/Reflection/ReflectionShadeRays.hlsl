// unx-kernel: cs_6_6 main
// unx-strict-fp
// Split/overflow and direction replay must agree before FP16 stores and ray traversal.
// unx-variants: SKY=0,1 CORNERS=0,1
// Hit shading of this frame's reflection rays in compute (ARCHITECTURE 2.6 revision 1), one thread per ray slot of the
// rays buffer (ReflectionRay.hlsli), dispatched indirectly for the slots allocated. Replays the job's direction, shades
// the hit (ReflectionShade.hlsli: surface, material, GI cache, S's VSM for the sun) or the sky, and stores the value; a
// hit the VSM does not hold keeps its sun term apart and queues one shadow ray (ReflectionShadow); a hit the VSM holds in
// a region that needs the penumbra filter keeps it apart too and queues the filter (ReflectionPenumbra: the same
// estimator in dense waves). Root constants: ReflectionRay.hlsli.
#define REFL_DEFER_PENUMBRA 1
#define GI_BATCH_CORNERS CORNERS
#define REFL_CHOICE_GIVEN 1  // the local light chosen by ReflectionLocalShadow (its value words)
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint slot = (group.y * 65535u + group.x) * 64u + lane;  // 2D dispatch (ReflectionRayArgs)
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    if (slot >= min(rays.Load(0), capacity)) return;
    const uint4 record = rays.Load4(reflRaysHitOffset(slot));
    const uint valueOffset = reflRaysValueOffset(capacity, slot);
    const uint layersUav = reflRayLayersUav(rays);  // reconstruction layers (UNX_NONE: off)
    g_reflHitFlags = reflHitFlags(rays);
    if (record.x == REFL_RAY_NONE)
    {
        rays.Store4(valueOffset, uint4(0, 0, 0, 0));  // valid bit clear
        return;
    }
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const ReflJob j = reflLoadJob(owner & 0x0FFFFFFFu);
    float3 dir;
    reflStoredDirection(j, rays, capacity, slot, dir);
    float3 radiance, sun = 0;
    float distanceToHit;
    float motion = 0;
    uint4 layer = reflLayerRay(0, 1, -dir, 0, false);  // sky, emitters: all in the base
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
        const ReflHitShade o = reflShadeHit(rtSceneSrvs(P[6], P[7]), cache, h, hit, reflRayOrigin(j.s), dir, j.coneWidth, j.coneSpread, reflLocalSeed(j, owner >> 28),
                                            ((record.x >> 30) & 1u) != 0, rays.Load2(valueOffset));  // the choice ReflectionLocalShadow stored
        radiance = o.radiance;
        distanceToHit = hit.t;
        motion = o.motion;
        layer = reflLayerRay(o.stochastic, o.albedo, o.hitNormal, hit.instance, o.surface, o.noData);
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
            // queued from the shadow queue's top, one atomic per wave (as the penumbra queue)
            sun = o.sunTerm;
            const uint lanes = WaveActiveCountBits(true), before = WavePrefixCountBits(true);
            uint first = 0;
            if (WaveIsFirstLane()) rays.InterlockedAdd(8, lanes, first);
            const uint index = WaveReadLaneFirst(first) + before;
            rays.Store4(reflRaysShadowOffset(capacity, index), uint4(asuint(o.shadowOrigin), slot));  // index < slots <= capacity
        }
    }
    rays.Store4(valueOffset, reflStoreValue(radiance, sun, distanceToHit, motion));
    if (layersUav != UNX_NONE)
    {
        RWByteAddressBuffer layers = ResourceDescriptorHeap[layersUav];
        layers.Store4(slot * REFL_LAYER_RAY_BYTES, layer);
    }
}
