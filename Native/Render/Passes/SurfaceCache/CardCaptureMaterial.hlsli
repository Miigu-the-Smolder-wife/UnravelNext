// The material of a card capture texel (r.card.capture; CardCapture.ps.hlsl states the targets): shared by the kernel
// that draws a card's instance from its source triangles (CardCapture.ps.hlsl) and the one that draws it from the
// cluster hierarchy's cut through V's raster service (CardCaptureCluster.ps.hlsl,
// surface_cache.mesh_cards_capture_clusters). Textures are sampled with the steps the kernel gives (SampleGrad), so a
// kernel whose material varies per primitive samples outside any derivative of its own flow.
#ifndef UNX_CARD_CAPTURE_MATERIAL_HLSLI
#define UNX_CARD_CAPTURE_MATERIAL_HLSLI
#include "Passes/SurfaceCache/CardCapture.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "MaterialModel.hlsli"
#include "Passes/SurfaceCache/CardLayout.hlsli"

// What a capture pixel knows of its surface.
struct CcSurface
{
    uint material;       // scene material (instance overrides applied)
    uint tableSrv;       // M's texture table SRV (UNX_NONE: material constants and the published textures only)
    float2 uv, uvDx, uvDy;                // material uv and its steps per capture pixel along x and y
    float3 normal;       // card space: x, y along the card's face, z toward the side the card is seen from
    float4 tangent;      // card space; w = the bitangent's sign in card space (xyz 0: none - no normal-map slope)
    float3 scaled, scaledDx, scaledDy;    // the mesh-space point x the instance's scale (metres) and its steps: triplanar materials
    uint direction;      // the card's direction (ccAxes)
    bool twoSided;       // a two-sided material: seen from its back it shows its back's normal
};
// The texel's material: covered = false where an alpha-tested material is cut out (the source-triangle kernel
// discards; the clusters' kernel writes it all the same - its depth pass has settled what the texel shows).
struct CcMaterial
{
    bool covered;
    float4 albedo;       // target 0
    float2 normal;       // target 1
    float4 emissive;     // target 2
};

// A material texture at the card texel's footprint: the uv's steps per capture pixel, given by the kernel (the source
// triangles' kernel takes its interpolants' derivatives, the clusters' kernel rebuilds them: CardCaptureCluster.ps.hlsl).
float4 ccSample(uint srv, bool clampAddress, float2 uv, float2 uvDx, float2 uvDy)
{
    Texture2D<float4> t = ResourceDescriptorHeap[srv];
    if (clampAddress) return t.SampleGrad(g_anisoClamp, uv, uvDx, uvDy);
    return t.SampleGrad(g_anisoWrap, uv, uvDx, uvDy);
}

// The reflectance of a layered material under uniform light, seen along the normal (mu = 1): what the card's albedo
// holds for it. reflectance: the base's (diffuse + 0.45 x f0), f0 and diffuse its parts.
//   thin film  f0 becomes F'(1) = lerp(f0, the film's table at mu = 1, the film's cover);
//   clearcoat  cover x (the coat lobe's albedo E_ms(1) + T(1) x the coat's mean transmission x (the base's reflectance +
//              the light the coat's inside returns to the base)) + (1 - cover) x the base - the terms HitShading.hlsli
//              gives a hit's cached light, with the base's specular in the capture's fully rough form;
//   sheen      the base x (1 - max(C) E_sh(1)) + C x E_sh(1).
float3 ccLayered(GpuMaterial m, uint cls, float3 baseColor, float metallic, float roughness, float3 reflectance)
{
    const GpuMaterialLayers layers = loadMaterialLayers(m.classFlags >> 16);
    const float3 diffuse = baseColor * (1 - metallic);
    if ((m.classFlags & MATERIAL_THIN_FILM) != 0 && layers.filmTable != 0)
    {
        g_modelFilmTable = layers.filmTable;
        const float3 f0 = lerp((0.08 * m.specular).xxx, baseColor, metallic);
        reflectance = diffuse + 0.45 * lerp(f0, modelFilmTable(1.0), saturate(layers.filmCoverage));
    }
    const ModelCoat coat = modelCoatOf(m);
    const ModelSheen sheen = modelSheenOf(m);
    if (coat.cover > 0)
    {
        ModelSurface s;
        s.cls = cls;
        s.baseColor = baseColor;
        s.roughness = roughness;
        s.metallic = metallic;
        s.specular = m.specular;
        s.transmission = m.transmission;
        const float coatAlbedo = modelCoatEms(coat, 1.0);
        const float through = (1 - coatAlbedo) * (1 - modelCoatLookup1(coat.coat * MODEL_COAT_STRIDE + 4096, coat.roughness));
        const float3 under = through * (reflectance + modelCoatReturned(s, coat, modelCoatRefractedCos(2.0 / 3.0, coat.eta)));
        reflectance = lerp(reflectance, under + coatAlbedo, saturate(coat.cover));
    }
    else if (any(sheen.color > 0))
        reflectance = modelSheenKeep(sheen, 1.0) * reflectance + sheen.color * modelSheenAlbedo(1.0, sheen.roughness);
    return reflectance;
}

CcMaterial ccMaterial(CcSurface i)
{
    CcMaterial o;
    o.covered = false;
    o.albedo = o.emissive = 0;
    o.normal = 0.5;
    const uint material = i.material, tableSrv = i.tableSrv;
    const GpuMaterial m = loadMaterial(material);
    const uint cls = materialClass(m);
    float3 baseColor = m.baseColor, emissive = m.emissive;
    float metallic = m.metallic, roughness = m.roughness, alpha = 1;
    float2 slope = 0;  // the normal map's mean slope (tangent space)
    float3 n = normalize(i.normal);
    if (cls == MATERIAL_TERRAIN && tableSrv != UNX_NONE)
    {
        // M's direct blending (MaterialTerrain.hlsli): the layers' base colours and metallic weighted by the splats
        const MTextureSet ts = mLoadTextureSet(tableSrv, material);
        const uint first = m.terrainLayers & 0xFFFFFFu, count = min(m.terrainLayers >> 24, 8u);
        const uint2 splatSize = loadTerrainLayer(first).splatSize;
        float weights[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        [unroll] for (uint s = 0; s < 2; ++s)
        {
            const uint srv = s == 0 ? ts.occlusion : asuint(ts.slopeRange), size = s == 0 ? splatSize.x : splatSize.y;
            if (srv == UNX_NONE || size == 0 || (s == 1 && count <= 4)) continue;
            const float2 texels = float2(size & 0xFFFFu, size >> 16), scale = (texels - 1) / texels;
            const float4 w = ccSample(srv, (ts.flags & (s == 0 ? M_TEX_BASE_COLOR : M_TEX_EMISSIVE)) != 0, i.uv * scale + 0.5 / texels, i.uvDx * scale, i.uvDy * scale);
            weights[4 * s] = w.x, weights[4 * s + 1] = w.y, weights[4 * s + 2] = w.z, weights[4 * s + 3] = w.w;
        }
        float sum = 0;
        uint k;
        [unroll] for (k = 0; k < 8; ++k)
        {
            if (k >= count) weights[k] = 0;
            sum += weights[k];
        }
        if (!(sum > 0))
        {
            weights[0] = 1;
            sum = 1;
        }
        float3 base = 0;
        float metal = 0, rough = 0;
        [unroll] for (k = 0; k < 8; ++k)
        {
            const float w = weights[k] / sum;
            if (!(w > 0)) continue;
            const GpuTerrainLayer layer = loadTerrainLayer(first + k);
            const GpuMaterial lm = loadMaterial(layer.material);
            const float2 uv = i.uv * layer.scale + layer.offset, uvDx = i.uvDx * layer.scale, uvDy = i.uvDy * layer.scale;
            float3 b = lm.baseColor;
            float mt = lm.metallic, rg = lm.roughness;
            if (lm.baseColorTexture != UNX_NONE) b *= ccSample(lm.baseColorTexture, (lm.textureClamp & M_TEX_BASE_COLOR) != 0, uv, uvDx, uvDy).rgb;
            if (lm.roughMetalTexture != UNX_NONE)
            {
                const float2 rm = ccSample(lm.roughMetalTexture, (lm.textureClamp & M_TEX_ROUGH_METAL) != 0, uv, uvDx, uvDy).rg;
                rg *= rm.r;
                mt *= rm.g;
            }
            base += w * b;
            metal += w * mt;
            rough += w * rg;
        }
        baseColor = base;
        metallic = metal;
        roughness = rough;
    }
    else if (cls == MATERIAL_CUT)
    {
        // the cut faces' object-space triplanar projection (CutFace.hlsli) at cutScale repeats per metre: the three
        // planes' samples weighted by the normal's components
        if (m.baseColorTexture != UNX_NONE)
        {
            float3 ax, ay, az;
            ccAxes(i.direction, ax, ay, az);
            const float3 meshNormal = normalize(ax * n.x + ay * n.y + az * n.z);
            float3 w = abs(meshNormal);
            w = w * w * w;
            w /= max(w.x + w.y + w.z, 1e-6);
            const float3 q = i.scaled * m.cutScale, qDx = i.scaledDx * m.cutScale, qDy = i.scaledDy * m.cutScale;
            const bool clampAddress = (m.textureClamp & M_TEX_BASE_COLOR) != 0;
            baseColor *= w.x * ccSample(m.baseColorTexture, clampAddress, q.yz, qDx.yz, qDy.yz).rgb +
                         w.y * ccSample(m.baseColorTexture, clampAddress, q.zx, qDx.zx, qDy.zx).rgb +
                         w.z * ccSample(m.baseColorTexture, clampAddress, q.xy, qDx.xy, qDy.xy).rgb;
        }
    }
    else
    {
        if (m.baseColorTexture != UNX_NONE)
        {
            const float4 c = ccSample(m.baseColorTexture, (m.textureClamp & M_TEX_BASE_COLOR) != 0, i.uv, i.uvDx, i.uvDy);
            baseColor *= c.rgb;
            alpha = c.a;
        }
        if (m.roughMetalTexture != UNX_NONE)
        {
            const float2 rm = ccSample(m.roughMetalTexture, (m.textureClamp & M_TEX_ROUGH_METAL) != 0, i.uv, i.uvDx, i.uvDy).rg;
            roughness *= rm.r;
            metallic *= rm.g;
        }
        if (m.emissiveTexture != UNX_NONE) emissive *= ccSample(m.emissiveTexture, (m.textureClamp & M_TEX_EMISSIVE) != 0, i.uv, i.uvDx, i.uvDy).rgb;
        if (tableSrv != UNX_NONE)
        {
            const MTextureSet ts = mLoadTextureSet(tableSrv, material);
            if (ts.moments != UNX_NONE) slope = (ccSample(ts.moments, (ts.flags & M_TEX_NORMAL) != 0, i.uv, i.uvDx, i.uvDy).xy * 2 - 1) * ts.slopeRange;
        }
    }
    const bool cutOut = m.alphaCutoff > 0 && alpha < m.alphaCutoff;  // (the kernel discards; the values below stand either way)
    if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) emissive = 0;

    if (any(slope != 0))
    {
        float3 t = i.tangent.xyz - n * dot(n, i.tangent.xyz);
        if (dot(t, t) > 1e-8)
        {
            t = normalize(t);
            const float3 b = i.tangent.w * cross(n, t);
            n = normalize(t * slope.x + b * slope.y + n);
        }
    }
    // the side the capture sees: a two-sided surface seen from its back shows its back's normal
    if (i.twoSided && n.z < 0) n = -n;
    if (!(n.z > 1e-3)) n = normalize(float3(n.xy, 1e-3));

    const float3 f0 = lerp((0.08 * m.specular).xxx, baseColor, metallic);
    float3 reflectance = baseColor * (1 - metallic) + 0.45 * f0;
    // (foliage has no layers: as the hits' shading)
    if ((m.classFlags & MATERIAL_LAYERED) != 0 && cls != MATERIAL_FOLIAGE) reflectance = ccLayered(m, cls, baseColor, metallic, roughness, reflectance);
    reflectance = saturate(reflectance);
    o.covered = !cutOut;
    o.albedo = float4(sqrt(reflectance), 1);
    o.normal = n.xy * 0.5 + 0.5;
    o.emissive = float4(min(max(emissive, 0.0) * MC_EMISSIVE_SCALE, 65000.0), 1);
    return o;
}

#endif
