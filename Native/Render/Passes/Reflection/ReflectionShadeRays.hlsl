// unx-kernel: cs_6_6 main
// unx-variants: SKY=0,1
// Hit shading of this frame's reflection rays in compute (ARCHITECTURE 2.6 revision 1), one thread per ray slot of the
// rays buffer (ReflectionRay.hlsli), dispatched indirectly for the slots allocated. Replays the job's direction, shades
// the hit (ReflectionShade.hlsli: surface, material, GI cache, S's VSM for the sun) or the sky, and stores the value; a
// hit the VSM does not hold keeps its sun term apart and queues one shadow ray (ReflectionShadow). Root constants:
// ReflectionRay.hlsli.
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
    bool moving = false;
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
        hit.instance = record.x & 0x7FFFFFFFu;
        hit.frontFace = record.x >> 31;
        hit.geometry = record.y;
        hit.primitive = record.z;
        hit.barycentrics = reflUnpackBarycentrics(rays.Load(reflRaysBaryOffset(capacity, slot)));
        hit.pad = 0;
        const ReflHitShade o = reflShadeHit(rtSceneSrvs(P[6], P[7]), cache, h, hit, reflRayOrigin(j.s), dir, j.coneWidth, j.coneSpread);
        radiance = o.radiance;
        distanceToHit = hit.t;
        moving = o.moving;
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
                                   f32tof16(s.b) | (1u << 16) | (moving ? (1u << 17) : 0u)));  // bit 16 valid, 17 moving hit
}
