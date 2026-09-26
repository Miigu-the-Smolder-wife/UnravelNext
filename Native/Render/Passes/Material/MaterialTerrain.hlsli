// M's Terrain material class (v1.74, FEATURES_GAME 9 direct blending; C5 cooked terrain tiles): up to 8 Standard
// layers weighted by two splat maps over the terrain's uv0. The weights are the splats' footprint-filtered samples
// normalised by their sum (layer 0 alone when the sum is 0); each layer with a weight is read at its own uv
// (uv0 x scale + offset) with M's footprint filter. Base colour, roughness and metallic are the weighted sums of the
// layers' values; the normal is the mixture of the layers' slope distributions (LEAN moments): mean = sum w m_i,
// variance = sum w (v_i + |m_i|^2) - |mean|^2 (exact for a mixture). Structural bound: 8 layers (2 + 3 x 8 taps).
#ifndef UNX_M_MATERIAL_TERRAIN_HLSLI
#define UNX_M_MATERIAL_TERRAIN_HLSLI

#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

struct MTerrainMaterial
{
    float3 baseColor;
    float roughness, metallic;
    float3 normal;     // world, unit (before the side and view rules)
    float variance;    // slope variance of the textures over the footprint (the geometric term is the caller's)
};

// The splat maps of a terrain material's texture set (TextureSystem.cpp): occlusion = splat 0, slopeRange's bits = splat 1.
// Texel centres sit at the terrain's corners (Unity's terrain splat convention, the heightmap's samples): an N-texel splat is
// read at uv0 x (N - 1) / N + 0.5 / N (N from the layer records, GpuScene::packTerrainLayers: no resinfo per pixel).
float4 mTerrainSplat(MTextureSet ts, uint k, uint size, MSurface s)
{
    const uint srv = k == 0 ? ts.occlusion : asuint(ts.slopeRange);
    if (srv == UNX_NONE || size == 0) return 0;
    Texture2D<float4> t = ResourceDescriptorHeap[srv];
    const float2 n = float2(size & 0xFFFFu, size >> 16), scale = (n - 1) / n;
    return mSampleGrad(t, (ts.flags & (k == 0 ? M_TEX_BASE_COLOR : M_TEX_EMISSIVE)) != 0, s.uv * scale + 0.5 / n, s.duvdx * scale, s.duvdy * scale);
}

// experiment: material.experiment_disable bits (1: textures, 2: normal map), as the resolve.
MTerrainMaterial mTerrainEvaluate(MSurface s, GpuMaterial m, uint tableSrv, uint experiment)
{
    const MTextureSet ts = mLoadTextureSet(tableSrv, s.material);
    const uint first = m.terrainLayers & 0xFFFFFFu, count = min(m.terrainLayers >> 24, 8u);
    float weights[8];
    const uint2 splatSize = loadTerrainLayer(first).splatSize;
    const float4 w0 = mTerrainSplat(ts, 0, splatSize.x, s), w1 = count > 4 ? mTerrainSplat(ts, 1, splatSize.y, s) : 0;
    weights[0] = w0.x, weights[1] = w0.y, weights[2] = w0.z, weights[3] = w0.w;
    weights[4] = w1.x, weights[5] = w1.y, weights[6] = w1.z, weights[7] = w1.w;
    float sum = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        if (i >= count) weights[i] = 0;
        sum += weights[i];
    }
    if (!(sum > 0))
    {
        weights[0] = 1;
        sum = 1;
    }
    const bool textures = (experiment & 1) == 0, normalMaps = (experiment & 2) == 0;
    const float3 B = s.tangentSign * cross(s.normal, s.tangent);
    float3 base = 0;
    float rough = 0, metal = 0, variance = 0;
    float2 mean = 0;
    float meanSquare = 0;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        const float w = weights[k] / sum;
        if (!(w > 0)) continue;
        const GpuTerrainLayer layer = loadTerrainLayer(first + k);
        const GpuMaterial lm = loadMaterial(layer.material);
        const MTextureSet lt = mLoadTextureSet(tableSrv, layer.material);
        const float2 uv = s.uv * layer.scale + layer.offset, dx = s.duvdx * layer.scale, dy = s.duvdy * layer.scale;
        float3 b = lm.baseColor;
        float r = lm.roughness, mt = lm.metallic;
        if (textures && lt.baseColor != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[lt.baseColor];
            b *= mSampleGrad(t, (lt.flags & M_TEX_BASE_COLOR) != 0, uv, dx, dy).rgb;
        }
        if (textures && lt.roughMetal != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[lt.roughMetal];
            const float2 rm = mSampleGrad(t, (lt.flags & M_TEX_ROUGH_METAL) != 0, uv, dx, dy).xy;
            r *= rm.x;
            mt *= rm.y;
        }
        float2 mk = 0;
        float vk = 0;
        if (normalMaps && lt.moments != UNX_NONE)
        {
            Texture2D<float4> t = ResourceDescriptorHeap[lt.moments];
            const MSlopeMoments mm = mNormalMoments(t, uv, dx, dy, lt.slopeRange, (lt.flags & M_TEX_NORMAL) != 0);
            mk = mm.mean;
            vk = mm.variance;
        }
        base += w * b;
        rough += w * r;
        metal += w * mt;
        mean += w * mk;
        meanSquare += w * (vk + dot(mk, mk));
    }
    MTerrainMaterial o;
    o.baseColor = base;
    o.roughness = rough;
    o.metallic = metal;
    o.normal = normalize(s.tangent * mean.x + B * mean.y + s.normal);
    o.variance = max(meanSquare - dot(mean, mean), 0.0);
    return o;
}

#endif
