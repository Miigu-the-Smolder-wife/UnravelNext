// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Shading kernel of the opaque classes (ARCHITECTURE 2.11; INTERFACES 5.6, 7, 8): one 8 x 8 tile of the class's tile
// list per group (ExecuteIndirect), pixels of other classes skipped. Per pixel, from the G-buffer, depth and material
// word (no vis-buffer re-derivation):
//   sun      model BRDF over the solar disk (ShadingCommon.hlsli shSunSpecular), S's shadow slot 0, S's transmittance;
//   GI       R's screen probes: diffuse irradiance x near occlusion; Foliage transmission from the back hemisphere;
//   specular R's reflection (G/M paths, planar mirrors) or the K path screenProbeRadiance with the lobe half-angle,
//            times the model's specular directional albedo;
//   emission material (x emissive texture, resolved per pixel);
//   air      S's aerial perspective between the camera and the surface (main view);
// then exposure, tone map and the final 4 B (OUTPUT=0) or linear radiance x exposure (OUTPUT=1).
// Classes without their own model yet (Subsurface, Water: INTERFACES 8.1 defines them before P3/P4) use this kernel.
// Local lights join through S's froxel lists (Froxel.hlsli) when S publishes them.
// P[0] = { gbuffer, depth, material word, color UAV }
// P[1] = { tile lists (raw), list offset (entries), shade class, emissive or UNX_NONE }
// P[2] = { shadow visibility, screen probes, reflection, GI cache (planar views) } (UNX_NONE = absent)
// P[3] = { atmosphere transmittance, multi-scatter, sky view, aerial } (UNX_NONE = absent)
// P[4] = { specular albedo LUT (float2 per grid point), texture table, 0, 0 }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
    const uint tile = tiles.Load(4 * (P[1].y + gid.x));
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    const uint word = words[pixel];
    const uint materialIndex = mWordMaterial(word);
    if (materialIndex == M_MATERIAL_SKY) return;
    const GpuMaterial m = loadMaterial(materialIndex);
    if (mShadeClass(materialClass(m)) != P[1].z) return;

    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    const GBufferSample g = decodeGBuffer(gbuffer[pixel]);
    const float linearZ = linearDepth(depthTex[pixel]);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;  // camera-relative position (D has unit depth along the view axis)
    const float3 worldPos = g_cameraPosition + offset;
    const float3 v = -normalize(D);
    const float3 n = g.normal;
    const float NoV = dot(n, v);

    ModelSurface s;
    s.cls = materialClass(m);
    s.baseColor = g.baseColor;
    s.roughness = g.roughness;
    s.metallic = mWordMetallic(word);
    s.specular = m.specular;
    s.transmission = m.transmission;
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);  // Lambert f_d before the Foliage split
    const float3 f0 = modelF0(s);
    const float alpha = modelAlpha(s.roughness);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 front = foliage ? diffuse * (1 - s.transmission) : diffuse;
    const float3 back = foliage ? diffuse * s.transmission : 0;

    AtmosphereSrvs atm;
    atm.transmittance = P[3].x;
    atm.multiScatter = P[3].y;
    atm.skyView = P[3].z;
    atm.aerial = P[3].w;
    const bool haveAtmosphere = atm.transmittance != UNX_NONE;

    float3 radiance = m.emissive;
    if (P[1].w != UNX_NONE && mLoadTextureSet(P[4].y, materialIndex).emissive != UNX_NONE)
    {
        Texture2D<float4> emissive = ResourceDescriptorHeap[P[1].w];
        radiance = emissive[pixel].rgb;  // material emissive x texture, resolved at the footprint
    }

    // ---- sun (INTERFACES 8.1: reflection on the viewer's side of the shading normal, Foliage transmission across it)
    const float3 l0 = normalize(g_sunDirection);
    const float3 E = haveAtmosphere ? atmosphereSunIlluminance(atm, worldPos) : g_sunIlluminance * g_sunColor;
    float sunVisibility = 1;
    if (P[2].x != UNX_NONE)
    {
        Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
        sunVisibility = shadowSlot(shadow[pixel], 0);
    }
    const float NoL = dot(n, l0);
    if (sunVisibility > 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));  // L_sun x solid angle of the disk
        float3 sun = 0;
        if (NoV > 0 && NoL > 0)
        {
            const float e = modelDirectionalAlbedo(NoV, s.roughness);
            const float3 compensation = 1 + f0 * (1 / e - 1);
            sun = front * NoL * cap + shSunSpecular(P[4].x, f0, s.roughness, alpha, compensation, n, v, NoV, l0, E, shPixelAngle(D, Dx));
        }
        else if (foliage && NoV * NoL < 0) sun = back * abs(NoL) * cap;
        radiance += sun * sunVisibility;
    }

    // ---- indirect (R): screen probes (main view) or the world cache (planar views). The viewer's side of the shading
    // normal reflects; Foliage also transmits what arrives on the other side.
    const float3 nv = NoV > 0 ? n : -n;
    const float3 r = reflect(-v, n);
    const float halfAngle = reflectionLobeHalfAngle(s.roughness, NoV);
    float3 irradiance = 0, irradianceBack = 0, incident = 0;
    if (g_viewKind == VIEW_MAIN && P[2].y != UNX_NONE)
    {
        ProbeSrvs probes;
        probes.probes = P[2].y;
        probes.occlusion = P[2].y;
        probes.pad0 = probes.pad1 = 0;
        const float4 e = screenProbeIrradiance(probes, pixel, nv, linearZ);
        irradiance = e.rgb * e.a;
        if (foliage) irradianceBack = screenProbeIrradiance(probes, pixel, -nv, linearZ).rgb * e.a;
        if (NoV > 0)
        {
            const float4 refl = P[2].z != UNX_NONE ? reflectionRadiance(P[2].z, pixel) : float4(0, 0, 0, 0);
            incident = refl.a > 0 ? refl.rgb : screenProbeRadiance(probes, pixel, n, linearZ, r, halfAngle);
        }
    }
    else if (g_viewKind == VIEW_PLANAR_REFLECTION && P[2].w != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = P[2].w;
        gi.hash = P[2].w;
        gi.pad0 = gi.pad1 = 0;
        irradiance = giCacheIrradiance(gi, worldPos, nv);
        if (foliage) irradianceBack = giCacheIrradiance(gi, worldPos, -nv);
        if (NoV > 0) incident = giCacheRadiance(gi, worldPos, r, halfAngle);
    }
    if (NoV > 0) radiance += front * irradiance + incident * shSpecularAlbedo(P[4].x, f0, NoV, s.roughness);
    radiance += back * irradianceBack;

    // ---- air between the camera and the surface (main view volume)
    if (haveAtmosphere && g_viewKind == VIEW_MAIN)
    {
        float3 inscatter, transmittance;
        atmosphereAerial(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearZ, inscatter, transmittance);
        radiance = radiance * transmittance + inscatter;
    }

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].w];
    color[pixel] = shEncodeOutput(radiance);
}
