// Scene textures as M publishes them in gpu::Material (INTERFACES 5.6, 6.3 v1.10; Passes/Material/TextureSystem.cpp).
// Owner: M. Readers: V (alpha test in the visibility and depth rasters), R (hit shading), M.
//   baseColorTexture   RGBA8 sRGB view (reads return linear colour), full mip chain. For a texture that an alpha-tested
//                      material uses: colour mips weighted by alpha (transparent texels never bleed into cut edges) and
//                      alpha mips that keep the fraction of texels passing that material's cutoff equal to level 0's
//                      (Castano 2010), so an alpha test sees the same coverage at every distance. Level 0 is the scene
//                      texture unchanged (the reference's mip 0).
//   roughMetalTexture  RG8: r = perceptual roughness factor, g = metallic factor; emissiveTexture RGBA8 sRGB or RGBA16F
//                      (multiplies Material.emissive, nits). Box mips of the linear values.
//   normalTexture      M's slope moments (RGBA16_UNORM, a range kept in M's material table): M-internal; other tracks
//                      use the interpolated vertex normal.
//   occlusionTexture   R8: baked ambient occlusion (1 = open), box mips. M's resolve stores it per pixel (the material
//                      word) and the shading kernel takes min(it, the short-range AO) for the indirect light.
//   textureClamp       bit per texture (MaterialTextureBit, MATERIAL_TEXTURE_* below): 1 = clamp addressing.
// The material's uv transform (scene::Material::uvScale / uvOffset / uvRotation; Scene.hlsli materialUv): the functions
// below take the mesh's uv and apply it, so the alpha test cuts the same shape in the rasters, the shadows and at ray
// hits; the ...At forms take a uv that is already the material's.
#ifndef UNX_M_MATERIAL_TEXTURES_HLSLI
#define UNX_M_MATERIAL_TEXTURES_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"

#define MATERIAL_TEXTURE_BASE_COLOR 1u
#define MATERIAL_TEXTURE_NORMAL 2u
#define MATERIAL_TEXTURE_ROUGH_METAL 4u
#define MATERIAL_TEXTURE_EMISSIVE 8u
#define MATERIAL_TEXTURE_OCCLUSION 16u

// Base colour texture (linear rgb, alpha = coverage) with the texture's addressing; 1 when the material has none.
float4 materialBaseColorGradAt(GpuMaterial m, float2 uv, float2 duvdx, float2 duvdy)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(m.baseColorTexture)];
    return (m.textureClamp & MATERIAL_TEXTURE_BASE_COLOR) ? t.SampleGrad(g_anisoClamp, uv, duvdx, duvdy) : t.SampleGrad(g_anisoWrap, uv, duvdx, duvdy);
}
float4 materialBaseColorGrad(GpuMaterial m, float2 uv, float2 duvdx, float2 duvdy)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    materialUvFootprint(m, uv, duvdx, duvdy);
    return materialBaseColorGradAt(m, uv, duvdx, duvdy);
}

float4 materialBaseColorLevelAt(GpuMaterial m, float2 uv, float lod)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(m.baseColorTexture)];
    return (m.textureClamp & MATERIAL_TEXTURE_BASE_COLOR) ? t.SampleLevel(g_anisoClamp, uv, lod) : t.SampleLevel(g_anisoWrap, uv, lod);
}
float4 materialBaseColorLevel(GpuMaterial m, float2 uv, float lod)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    return materialBaseColorLevelAt(m, materialUv(m, uv), lod);
}

// Dithered opacity (scene::Material::alphaDither): the alpha test's threshold at a pixel of a view's raster - the
// cutoff + noise - 0.5 (held to [1/255, 1]; the cutoff 0.5 gives a coverage equal to the alpha), the noise the
// interleaved gradient pattern, moved every frame while the temporal upscale accumulates (g_upscaleRatio > 0) and still
// otherwise. A material without the flag: its cutoff.
float materialAlphaThreshold(GpuMaterial m, float2 pixelPosition)
{
    if (m.inputs == UNX_NONE || (loadMaterialInputs(m.inputs).flags & MATERIAL_INPUT_DITHER) == 0) return m.alphaCutoff;
    const float2 p = pixelPosition + (g_upscaleRatio > 0 ? 5.588238 * (float)(g_frameIndex & 63u) : 0.0);
    const float noise = frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
    return clamp(m.alphaCutoff + noise - 0.5, 1.0 / 255.0, 1.0);
}

#endif
