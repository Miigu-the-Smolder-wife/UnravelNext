// unx-kernel: lib_6_6 main
// Visibility of the local-light samples of this frame's reflection hits (HitLocalLights.hlsli), one ray generation thread
// per allocated ray slot, before the compute hit shading: the slot's hit point is rebuilt exactly as ReflectionShade
// builds it (rtSurface of the stored hit record), the same light sample is drawn (reflLocalSeed of the slot's owner), and
// one shadow ray is traced toward it; a visible sample sets bit 30 of the hit record's first word (instance ids are 24
// bits), which ReflectionShadeRays reads. Lights that cast no shadow, and samples the hit's BRDF gives 0, need no ray. Root constants: ReflectionRay.hlsli.
#define SKY 1  // no sky lookups here
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiInternal.hlsli"

uint reflLocalSeed(uint owner) { return giRandom(owner * 7919u + (P[5].x & 0xFFFFFFu) * 104729u + 31u); }

[shader("raygeneration")]
void ReflectionLocalShadowGen()
{
    const uint slot = DispatchRaysIndex().x;
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    const uint4 record = rays.Load4(reflRaysHitOffset(slot));
    if (record.x == REFL_RAY_MISS || record.x == REFL_RAY_NONE) return;
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const ReflJob j = reflLoadJob(owner & 0x0FFFFFFFu);
    float3 dir;
    reflRayDirection(j, owner >> 28, dir);
    RtHit hit;
    hit.t = asfloat(record.w);
    hit.instance = record.x & 0x00FFFFFFu;
    hit.frontFace = record.x >> 31;
    hit.geometry = record.y;
    hit.primitive = record.z;
    hit.barycentrics = reflUnpackBarycentrics(rays.Load(reflRaysBaryOffset(capacity, slot)));
    hit.pad = 0;
    if (hit.instance == RT_INSTANCE_EMITTER) return;
    const RtSceneSrvs scene = rtScene();
    // The sample needs only the hit point (rtSurface's position, origin + direction t): drawn before the surface is
    // rebuilt, so hits whose sample casts no shadow skip the rebuild.
    const float3 origin = reflRayOrigin(j.s);
    const uint seed = reflLocalSeed(owner);
    const RtLocalSample ls = rtLocalLightSample(scene, origin + dir * hit.t, giUnit(seed), giUnit(seed + 1), giUnit(seed + 2), 0);  // the choice only
    if (!ls.valid || !ls.castShadow) return;
    const RtSurface s = rtSurface(scene, hit, origin, dir);
    // A sample below the hit's shading normal adds nothing to the hit (rtLocalLightBrdfCos is 0 for N.L <= 0 unless the
    // material is Foliage, which transmits): its visibility is not read, no ray (the same s.normal as the shading's).
    if (dot(s.normal, ls.wi) <= 0 && materialClass(loadMaterial(s.material)) != MATERIAL_FOLIAGE) return;
    if (rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, 1e-3 + 2e-4 * distance(s.position, g_cameraPosition)), RT_MASK_REFLECTION))
        rays.Store(reflRaysHitOffset(slot), record.x | (1u << 30));
}
