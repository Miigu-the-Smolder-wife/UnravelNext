// unx-kernel: ps_6_6 main
// r.card.capture (CardCapture.hlsli): the card texel's material. Targets:
//   0 albedo   RGBA8  sqrt(diffuse reflectance + 0.45 x specular colour) - what a rough bounce off the surface carries
//                     (Unreal's card albedo: DiffuseColor + SpecularColor x 0.45), a = 1
//   1 normal   RG8    the shading normal's x, y in card space x 0.5 + 0.5 (z >= 0: toward the capture side)
//   2 emissive R11G11B10F  emission in nits x MC_EMISSIVE_SCALE (0 for a visible-only emissive: its analytic light
//                     lights the scene, INTERFACES v1.92)
// Textures by the pixel's own derivatives (the card's texel footprint). Material classes: Standard, Foliage,
// Subsurface (their base layer); Terrain (the splat-weighted layers' base colour and metallic); Cut (triplanar base
// colour). Water, Glass and Hair are not in the cache (their draws are skipped on the CPU).
#include "Passes/SurfaceCache/CardCapture.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/SurfaceCache/CardLayout.hlsli"

struct CardPixel
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 emissive : SV_Target2;
};

float4 ccSample(uint srv, bool clampAddress, float2 uv)
{
    Texture2D<float4> t = ResourceDescriptorHeap[srv];
    if (clampAddress) return t.Sample(g_anisoClamp, uv);
    return t.Sample(g_anisoWrap, uv);
}

CardPixel main(CardVertex i)
{
    const uint material = P[0].w, tableSrv = P[4].x;
    const GpuMaterial m = loadMaterial(material);
    const uint cls = materialClass(m);
    float3 baseColor = m.baseColor, emissive = m.emissive;
    float metallic = m.metallic, alpha = 1;
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
            const float4 w = ccSample(srv, (ts.flags & (s == 0 ? M_TEX_BASE_COLOR : M_TEX_EMISSIVE)) != 0, i.uv * scale + 0.5 / texels);
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
        float metal = 0;
        [unroll] for (k = 0; k < 8; ++k)
        {
            const float w = weights[k] / sum;
            if (!(w > 0)) continue;
            const GpuTerrainLayer layer = loadTerrainLayer(first + k);
            const GpuMaterial lm = loadMaterial(layer.material);
            const float2 uv = i.uv * layer.scale + layer.offset;
            float3 b = lm.baseColor;
            float mt = lm.metallic;
            if (lm.baseColorTexture != UNX_NONE) b *= ccSample(lm.baseColorTexture, (lm.textureClamp & M_TEX_BASE_COLOR) != 0, uv).rgb;
            if (lm.roughMetalTexture != UNX_NONE) mt *= ccSample(lm.roughMetalTexture, (lm.textureClamp & M_TEX_ROUGH_METAL) != 0, uv).g;
            base += w * b;
            metal += w * mt;
        }
        baseColor = base;
        metallic = metal;
    }
    else if (cls == MATERIAL_CUT)
    {
        // the cut faces' object-space triplanar projection (CutFace.hlsli) at cutScale repeats per metre: the three
        // planes' samples weighted by the normal's components
        if (m.baseColorTexture != UNX_NONE)
        {
            float3 ax, ay, az;
            ccAxes(P[2].w & 7u, ax, ay, az);
            const float3 meshNormal = normalize(ax * n.x + ay * n.y + az * n.z);
            float3 w = abs(meshNormal);
            w = w * w * w;
            w /= max(w.x + w.y + w.z, 1e-6);
            const float3 q = i.scaled * m.cutScale;
            const bool clampAddress = (m.textureClamp & M_TEX_BASE_COLOR) != 0;
            baseColor *= w.x * ccSample(m.baseColorTexture, clampAddress, q.yz).rgb + w.y * ccSample(m.baseColorTexture, clampAddress, q.zx).rgb +
                         w.z * ccSample(m.baseColorTexture, clampAddress, q.xy).rgb;
        }
    }
    else
    {
        if (m.baseColorTexture != UNX_NONE)
        {
            const float4 c = ccSample(m.baseColorTexture, (m.textureClamp & M_TEX_BASE_COLOR) != 0, i.uv);
            baseColor *= c.rgb;
            alpha = c.a;
        }
        if (m.roughMetalTexture != UNX_NONE) metallic *= ccSample(m.roughMetalTexture, (m.textureClamp & M_TEX_ROUGH_METAL) != 0, i.uv).g;
        if (m.emissiveTexture != UNX_NONE) emissive *= ccSample(m.emissiveTexture, (m.textureClamp & M_TEX_EMISSIVE) != 0, i.uv).rgb;
        if (tableSrv != UNX_NONE)
        {
            const MTextureSet ts = mLoadTextureSet(tableSrv, material);
            if (ts.moments != UNX_NONE) slope = (ccSample(ts.moments, (ts.flags & M_TEX_NORMAL) != 0, i.uv).xy * 2 - 1) * ts.slopeRange;
        }
    }
    if (m.alphaCutoff > 0 && alpha < m.alphaCutoff) discard;
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
    if ((((P[2].w >> 8) & 1u) != 0) && n.z < 0) n = -n;
    if (!(n.z > 1e-3)) n = normalize(float3(n.xy, 1e-3));

    const float3 f0 = lerp((0.08 * m.specular).xxx, baseColor, metallic);
    const float3 reflectance = saturate(baseColor * (1 - metallic) + 0.45 * f0);
    CardPixel o;
    o.albedo = float4(sqrt(reflectance), 1);
    o.normal = n.xy * 0.5 + 0.5;
    o.emissive = float4(min(max(emissive, 0.0) * MC_EMISSIVE_SCALE, 65000.0), 1);
    return o;
}
