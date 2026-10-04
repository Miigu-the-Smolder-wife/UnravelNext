// Water at a ray hit (the Lumen path's reflection and refraction rays): a ray that meets a water surface sees that
// surface, not an opaque sheet of the material's base colour and not the bed as if the basin were dry. The surfaces:
//   W's triangle streams (basins, fluids: RayScene's instances RT_INSTANCE_STREAM_BASE + slot, mask RT_MASK_FLUID) - the
//       normal from the stream's vertices, the water from the stream's scene material (R's stream table:
//       ReflectionSystem.cpp - word `slot` the vertex buffer's SRV, word 64 + slot the material);
//   scene meshes of a Water material (a lake's sheet; in the reflection mask as every mesh).
// From the air, the ray's light is
//   F x the surroundings' light along the mirror direction + (1 - F) / n^2 x what the refracted ray finds, through the
//   water's extinction along it + the water's own light,
// with F the exact unpolarised Fresnel term (WaterShading.hlsli). The mirror direction gets no ray of its own (a
// reflection inside a reflection): its stand-in is the hit's ambient light as a uniform radiance (the frame's indirect
// light at the hit, LumenHitIndirect.hlsli: the translucency volume, else the radiance cache's probes, else the far
// sky's - what the cards' hits use as their lobe's light), so a bath indoors mirrors the room's light and not the sky
// above the roof. The water's own light is what a water that scatters returns of that ambient light, as a Lambert
// surface of the diffusion limit's albedo (waterDiffuseAlbedo; the sun's part needs a shadow ray the thread does not
// have: left out). The refracted ray is the caller's to follow (one more trace; the bed is shaded as any hit).
// From inside the water (a ray that started under the surface): the callers' media walks own the exit; a kernel
// without one takes the ambient stand-in for both the mirrored and the transmitted part.
// The includer is a ray library with LumenHitIndirect.hlsli and GiSky.hlsli (ReflectionLumenHit.hlsli brings them).
#ifndef UNX_RT_HIT_WATER_HLSLI
#define UNX_RT_HIT_WATER_HLSLI
#include "Passes/Common/TriangleStream.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "Passes/Water/WaterShading.hlsli"

#ifndef RT_INSTANCE_STREAM_BASE
#define RT_INSTANCE_STREAM_BASE 0xFFFF00u
#endif
#ifndef RT_MASK_FLUID
#define RT_MASK_FLUID 0x8u
#endif

struct RtWaterSurface
{
    float3 position;
    float3 normal;         // unit, on the side the ray comes from
    float3 sigmaT, sigmaS; // the water's extinction and scattering (1/m)
    float g, ior;
    bool fromAir;          // the ray meets the surface from outside the water
};

bool rtStreamHit(RtHit hit) { return hit.t >= 0 && hit.instance >= RT_INSTANCE_STREAM_BASE && hit.instance < RT_INSTANCE_STREAM_BASE + 64u; }

// Whether the hit is a water surface, and that surface. streamTable: R's stream table (UNX_NONE: none - the ray's mask
// then leaves RT_MASK_FLUID out and only meshes of a Water material are met).
bool rtWaterSurface(RtSceneSrvs scene, RtHit hit, float3 origin, float3 direction, uint streamTable, out RtWaterSurface w)
{
    w = (RtWaterSurface)0;
    if (hit.t < 0) return false;
    uint material = UNX_NONE;
    if (rtStreamHit(hit))
    {
        if (streamTable == UNX_NONE) return false;
        const uint slot = hit.instance - RT_INSTANCE_STREAM_BASE;
        ByteAddressBuffer table = ResourceDescriptorHeap[streamTable];
        ByteAddressBuffer v = ResourceDescriptorHeap[table.Load(slot * 4)];
        material = table.Load((64 + slot) * 4);
        const uint3 vertexIds = triangleStreamVertexIds(table.Load((128u + slot) * 4u), hit.primitive);  // (32 B vertices, normals at +16)
        const float3 b = rtBary(hit.barycentrics);
        // (the stream's normals point out of the water)
        const float3 n = normalize(asfloat(v.Load3(32 * vertexIds.x + 16)) * b.x + asfloat(v.Load3(32 * vertexIds.y + 16)) * b.y + asfloat(v.Load3(32 * vertexIds.z + 16)) * b.z);
        w.fromAir = dot(n, direction) < 0;
        w.normal = w.fromAir ? n : -n;
    }
    else
    {
        if (!rtMeshHit(hit)) return false;
        // (the material from the hit's records first: every other hit pays one material load here, not the surface's)
        GpuInstance inst;
        GpuMesh mesh;
        RtGeometry geometry;
        material = rtMaterial(scene, hit, inst, mesh, geometry);
        if (materialClass(loadMaterial(material)) != MATERIAL_WATER) return false;
        const RtSurface s = rtSurface(scene, hit, origin, direction);
        w.fromAir = s.frontFace;  // (a sheet's front is its air side)
        w.normal = s.normal;      // (rtSurface turns it to the ray's side)
    }
    w.position = origin + direction * hit.t;
    // the material's water: baseColor = the transmittance over 1 m, + the scattering (WaterSurface.hlsli's reading)
    float3 base = float3(0.712, 0.945, 0.991);
    w.ior = kWaterIor;
    if (material != UNX_NONE)
    {
        const GpuMaterial m = loadMaterial(material);
        base = m.baseColor;
        w.sigmaS = m.hairAbsorption;
        w.g = clamp(m.hairBetaN, -0.99, 0.99);
        if (m.ior > 1.0001) w.ior = m.ior;
    }
    w.sigmaT = -log(clamp(base, 1e-6, 1.0)) + w.sigmaS;
    return true;
}

// The hit's ambient light as a uniform radiance (the header's stand-in): the frame's indirect irradiance on the
// surface's air side / pi.
float3 rtWaterAmbient(RtWaterSurface w, uint cardFrame, uint seed)
{
    const float3 up = w.fromAir ? w.normal : -w.normal;
    const float4 e = lhiIrradiance(lhiSources(cardFrame), w.position, up, seed);
    const float3 irradiance = e.a > 0 ? e.rgb : giFarSkyIrradiance(w.position, up, lhiRules(cardFrame).farStart);
    return irradiance * (1.0 / 3.14159265358979);
}

// A ray from the air at the surface: the part of its light that needs no further ray (the mirrored surroundings and
// the water's own light), the refracted direction and the weight of what that direction finds at the surface (before
// the water's extinction along it: x exp(-sigmaT x the path)).
float3 rtWaterFromAir(RtWaterSurface w, float3 direction, float3 ambient, out float3 refracted, out float weight)
{
    const float cosI = saturate(dot(w.normal, -direction));
    const float F = waterFresnel(cosI, 1.0 / w.ior);
    weight = (1 - F) / (w.ior * w.ior);
    if (!waterRefract(-direction, w.normal, 1.0 / w.ior, refracted)) refracted = direction;  // (never from the air)
    float3 light = F * ambient;
    // 0.934 of a uniform light enters a level surface; the water returns its diffuse albedo of it as a Lambert surface
    if (any(w.sigmaS > 0)) light += weight * waterDiffuseAlbedo(w.sigmaT - w.sigmaS, w.sigmaS, w.g, 1e4) * (0.934 * ambient);
    return light;
}
#endif
