// unx-kernel: lib_6_6 main
// unx-strict-fp
// Split/overflow and direction replay must agree before FP16 stores and ray traversal.
// Visibility of the local-light samples of this frame's reflection hits (HitLocalLights.hlsli), one ray generation thread
// per allocated ray slot, before the compute hit shading: the slot's hit point is rebuilt exactly as ReflectionShade
// builds it (rtSurface of the stored hit record), the same light sample is drawn (reflLocalSeed of the slot's job and ray), and
// one shadow ray is traced toward it; a visible sample sets bit 30 of the hit record's first word (instance ids are 24
// bits), which ReflectionShadeRays reads. Lights that cast no shadow, and samples the hit's BRDF gives 0, need no ray. Root constants: ReflectionRay.hlsli.
#define SKY 1  // no sky lookups here
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiInternal.hlsli"


[shader("raygeneration")]
void ReflectionLocalShadowGen()
{
    const uint slot = DispatchRaysIndex().x + reflBand() * REFL_BAND;
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    const uint4 record = rays.Load4(reflRaysHitOffset(slot));
    if (record.x == REFL_RAY_MISS || record.x == REFL_RAY_NONE) return;
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const ReflJob j = reflLoadJob(owner & 0x0FFFFFFFu);
    float3 dir;
    reflStoredDirection(j, rays, capacity, slot, dir);
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
    const uint seed = reflLocalSeed(j, owner >> 28);
    const float3 x = origin + dir * hit.t;
    // reflection.hit_oriented_lights (the rays header's flags, ReflectionShade.hlsli REFL_HIT_ORIENTED = 2): the choice
    // weighs the lights by the hit's orientation, so the surface is rebuilt before it (every hit, not only those whose
    // sample casts a shadow: the pass's work grows by those rebuilds).
    // (one function for both settings: with transmits = true it is rtLocalLightChoose's choice, bit for bit)
    float3 orientation = 0;
    bool transmits = true;
    if ((reflHitFlags(rays) & 2u) != 0)
    {
        const RtSurface so = rtSurface(scene, hit, origin, dir);
        orientation = so.normal;
        transmits = materialClass(loadMaterial(so.material)) == MATERIAL_FOLIAGE;
    }
    const RtLocalChoice choice = rtLocalLightChooseOriented(scene, x, orientation, transmits, giUnit(seed));
    // The choice to the shading pass through the slot's value words (not written before r.refl.shade stores the value):
    // it draws on the same light without walking the cell's lights again (ReflectionShade.hlsli).
    rays.Store2(reflRaysValueOffset(capacity, slot), rtPackLocalChoice(choice));
    const RtLocalSample ls = rtLocalLightFinish(scene, choice, x, giUnit(seed + 1), giUnit(seed + 2), 0);  // (visibility: the footprint does not matter)
    if (!ls.valid || !ls.castShadow) return;
    const RtSurface s = rtSurface(scene, hit, origin, dir);
    // A sample below the hit's shading normal adds nothing to the hit (rtLocalLightBrdfCos is 0 for N.L <= 0 unless the
    // material is Foliage, which transmits): its visibility is not read, no ray (the same s.normal as the shading's).
    if (dot(s.normal, ls.wi) <= 0 && materialClass(loadMaterial(s.material)) != MATERIAL_FOLIAGE) return;
    if (rtVisible(scene, rtLocalShadowRay(s.position, s.geometricNormal, ls, 1e-3 + 2e-4 * distance(s.position, g_cameraPosition)), RT_MASK_REFLECTION))
        rays.Store(reflRaysHitOffset(slot), record.x | (1u << 30));
}
