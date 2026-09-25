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
//   air      S's air volume between the camera and the surface (main view, atmosphereAirView: atmosphere, casters'
//            shadows in the air, local lights' air) and the sun's illuminance at the surface;
// then exposure, tone map and the final 4 B (OUTPUT=0) or linear radiance x exposure (OUTPUT=1).
// Classes without their own model yet (Subsurface, Water: INTERFACES 8.1 defines them before P3/P4) use this kernel.
// Local lights: the pixel's froxel list (S, Froxel.hlsli), punctual lights exactly (INTERFACES 8.2); shadow-casting
// lights take S's slots 1-3 in list order (7.3). Area lights need the LTC tables (next). Planar reflection views have
// no froxel lists (they are the main view's) and so no local lights yet.
// P[0] = { gbuffer, depth, material word, color UAV }
// P[1] = { tile lists (raw), list offset (entries), shade class, emissive or UNX_NONE }
// P[2] = { shadow visibility, screen probes, reflection, GI cache (planar views) } (UNX_NONE = absent)
// P[3] = { atmosphere transmittance, multi-scatter, sky view, aerial } (UNX_NONE = absent)
// P[4] = { specular albedo LUT (float2 per grid point), texture table, experiment mask (0; shading.toml), vis id SRV }
// P[5] = { froxel lights (raw), 0 } (UNX_NONE = absent)
// P[6] = { edge cos angle, edge footprint tolerance, edge distance tolerance (floats), edge args UAV (raw) }
// P[7] = { edge radiance UAV (RGBA16F), 0, 0, edge pixel list UAV (raw) }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

struct ShadedPixel
{
    float3 radiance;  // linear, before exposure
    float linearZ;
    float3 normal;    // shading normal (G-buffer)
};

ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, Texture2D<uint2> gbuffer, Texture2D<float> depthTex);

// Edge detection shares the tile's samples: each lane publishes its vis id and edge sample, so a neighbour inside the
// tile costs no loads; neighbours outside it are read from the screen buffers. A lane of another class (or the sky)
// shows another material, which is an edge before any geometry.
#define EDGE_OUTSIDE 0xFFFFFFFFu
groupshared uint gsVis[64];
groupshared uint gsMaterial[64];  // EDGE_OUTSIDE: outside the view
groupshared float3 gsPosition[64];
groupshared float3 gsNormal[64];
groupshared float gsFootprint[64];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer tiles = ResourceDescriptorHeap[P[1].x];
    const uint tile = tiles.Load(4 * (P[1].y + gid.x));
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[4].w];
    const bool inView = all(pixel < uint2(g_viewWidth, g_viewHeight));
    const uint word = inView ? words[pixel] : M_MATERIAL_SKY;
    const uint materialIndex = mWordMaterial(word);
    bool active = inView && materialIndex != M_MATERIAL_SKY;
    const GpuMaterial m = loadMaterial(active ? materialIndex : 0);
    active = active && mShadeClass(materialClass(m)) == P[1].z;
    ShadedPixel sp = (ShadedPixel)0;
    if (active) sp = shadeSurface(pixel, word, materialIndex, m, words, gbuffer, depthTex);

    // ---- edge (E) pixels keep their exposed linear radiance for the composite (EdgeComposite.hlsl)
    if (P[7].x == UNX_NONE || (P[4].z & 256) != 0) return;
    const uint lane = tid.y * M_TILE + tid.x;
    const EdgePixel c = edgeSample(pixel, materialIndex, sp.linearZ, sp.normal);
    const uint vc = inView ? visIds[pixel] : VIS_NONE;
    gsVis[lane] = vc;
    gsMaterial[lane] = inView ? materialIndex : EDGE_OUTSIDE;
    gsPosition[lane] = c.position;
    gsNormal[lane] = c.normal;
    gsFootprint[lane] = c.footprint;
    GroupMemoryBarrierWithGroupSync();
    bool isEdge = false;
    if (active)
    {
        const EdgeParams ep = { asfloat(P[6].x), asfloat(P[6].y), asfloat(P[6].z) };
        [unroll] for (uint k = 0; k < 9; ++k)
        {
            if (k == 4 || isEdge) continue;
            const int2 o = int2(int(k % 3) - 1, int(k / 3) - 1);
            const int2 q = int2(pixel) + o;
            if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) continue;
            const int2 t = int2(tid) + o;
            EdgePixel e = (EdgePixel)0;
            if (all(t >= 0) && all(t < int(M_TILE)))
            {
                const uint l = uint(t.y) * M_TILE + uint(t.x);
                if (gsVis[l] == vc) continue;
                e.material = gsMaterial[l];
                e.sky = false;
                e.position = gsPosition[l];
                e.normal = gsNormal[l];
                e.footprint = gsFootprint[l];
            }
            else
            {
                if (visIds[uint2(q)] == vc) continue;
                e.material = mWordMaterial(words[uint2(q)]);
                if (e.material == c.material) e = edgeSample(uint2(q), e.material, linearDepth(depthTex[uint2(q)]), octDecode(gbuffer[uint2(q)].x));
            }
            isEdge = e.material != c.material || !edgeSameSurface(c, e, ep);
        }
        if (isEdge)
        {
            RWTexture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[7].x];
            edgeRadiance[pixel] = float4(sp.radiance * g_exposure, 1);
        }
    }
    edgeAppendPixel(pixel, isEdge, P[7].w, P[6].w);
}

// Shades one pixel of this class (writes the output) and returns what edge detection needs.
ShadedPixel shadeSurface(uint2 pixel, uint word, uint materialIndex, GpuMaterial m, Texture2D<uint> words, Texture2D<uint2> gbuffer, Texture2D<float> depthTex)
{
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
    // Main view: S's air volume gives the air between the camera and the surface and the sun's illuminance at it in one
    // lookup (atmosphereAirView); secondary views have no air volume (the grid is the main view's).
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (haveAtmosphere)
    {
        if (g_viewKind == VIEW_MAIN && (P[4].z & 8) == 0)
            atmosphereAirView(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    float sunVisibility = 1;
    if (P[2].x != UNX_NONE)
    {
        Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
        sunVisibility = shadowSlot(shadow[pixel], 0);
    }
    const float NoL = dot(n, l0);
    const uint experiment = P[4].z;
    if (sunVisibility > 0 && (experiment & 16) == 0)
    {
        const float3 cap = E * (2 / (1 + cos(g_sunAngularRadius)));  // L_sun x solid angle of the disk
        // The disk's parts above and below the shading normal's horizon (cap-averaged clipped cosines).
        const float above = shCapCosine(NoL), below = shCapCosine(-NoL);
        float3 sun = 0;
        if (NoV > 0)
        {
            if (above > 0)
            {
                const float e = modelDirectionalAlbedo(NoV, s.roughness);
                const float3 compensation = 1 + f0 * (1 / e - 1);
                sun = front * above * cap;
                if (experiment & 1) sun += NoL > 0 ? shSpecular(f0, alpha, compensation, n, v, l0, NoV, NoL) * NoL * cap : 0;
                else sun += shSunSpecular(P[4].x, f0, s.roughness, alpha, compensation, n, v, NoV, l0, E, shPixelAngle(D, Dx));
            }
            if (foliage) sun += back * below * cap;
        }
        else if (foliage) sun = back * above * cap;  // viewer behind the shading normal: only light crossing the leaf
        radiance += sun * sunVisibility;
    }

    // ---- local lights (main view: S's froxel lists)
    FroxelSrvs froxels;
    froxels.lights = P[5].x;
    froxels.lightIndices = P[5].x;
    froxels.scattering = UNX_NONE;
    froxels.pad = 0;
    if (froxels.lights != UNX_NONE && g_viewKind == VIEW_MAIN && (experiment & 32) == 0)
    {
        uint shadowPacked = 0xFFFFFFFFu;  // all slots lit when S publishes no visibility
        if (P[2].x != UNX_NONE)
        {
            Texture2D<uint> shadow = ResourceDescriptorHeap[P[2].x];
            shadowPacked = shadow[pixel];
        }
        const float e = modelDirectionalAlbedo(max(NoV, 1e-4), s.roughness);
        const float3 compensation = 1 + f0 * (1 / e - 1);
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        uint shadowOrdinal = 0;
        for (uint i = 0; i < range.y; ++i)
        {
            const GpuLight light = loadLight(froxelLight(froxels, range.x + i));
            float visibility = 1;
            if (lightCastsShadow(light))
            {
                ++shadowOrdinal;
                if (shadowOrdinal <= 3) visibility = shadowSlot(shadowPacked, shadowOrdinal);
            }
            if (visibility <= 0 || lightType(light) > LIGHT_SPOT) continue;
            float3 l;
            const float3 E = shPunctualIlluminance(light, (light.position - g_cameraPosition) - offset, l);
            const float cosL = dot(n, l);
            float3 f = 0;
            if (NoV > 0 && cosL > 0) f = front + shSpecular(f0, alpha, compensation, n, v, l, NoV, cosL);
            else if (foliage && NoV * cosL < 0) f = back;
            radiance += f * E * (abs(cosL) * visibility);
        }
    }

    // ---- indirect (R): screen probes (main view) or the world cache (planar views). The viewer's side of the shading
    // normal reflects; Foliage also transmits what arrives on the other side.
    const float3 nv = NoV > 0 ? n : -n;
    const float3 r = reflect(-v, n);
    const float halfAngle = reflectionLobeHalfAngle(s.roughness, NoV);
    float3 irradiance = 0, irradianceBack = 0, incident = 0;
    if (g_viewKind == VIEW_MAIN && P[2].y != UNX_NONE && (experiment & 6) != 6)
    {
        ProbeSrvs probes;
        probes.probes = P[2].y;
        probes.occlusion = P[2].y;
        probes.pad0 = probes.pad1 = 0;
        if ((experiment & 2) == 0)
        {
            const float4 e = screenProbeIrradiance(probes, pixel, nv, linearZ);
            irradiance = e.rgb * e.a;
            if (foliage) irradianceBack = screenProbeIrradiance(probes, pixel, -nv, linearZ).rgb * e.a;
        }
        if (NoV > 0 && (experiment & 4) == 0)
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

    // ---- air between the camera and the surface (S's air volume: atmosphere, shadowed air, local lights' air)
    radiance = radiance * airTransmittance + airInscatter;

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[0].w];
    color[pixel] = shEncodeOutput(radiance);
    ShadedPixel o;
    o.radiance = radiance;
    o.linearZ = linearZ;
    o.normal = n;
    return o;
}
