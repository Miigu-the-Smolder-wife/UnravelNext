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
//   occlusionTexture   UNX_NONE (its use is not defined in INTERFACES 8.1 v1).
//   textureClamp       bit per texture (MaterialTextureBit, MATERIAL_TEXTURE_* below): 1 = clamp addressing.
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
float4 materialBaseColorGrad(GpuMaterial m, float2 uv, float2 duvdx, float2 duvdy)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    Texture2D<float4> t = ResourceDescriptorHeap[m.baseColorTexture];
    return (m.textureClamp & MATERIAL_TEXTURE_BASE_COLOR) ? t.SampleGrad(g_anisoClamp, uv, duvdx, duvdy) : t.SampleGrad(g_anisoWrap, uv, duvdx, duvdy);
}

float4 materialBaseColorLevel(GpuMaterial m, float2 uv, float lod)
{
    if (m.baseColorTexture == UNX_NONE) return 1;
    Texture2D<float4> t = ResourceDescriptorHeap[m.baseColorTexture];
    return (m.textureClamp & MATERIAL_TEXTURE_BASE_COLOR) ? t.SampleLevel(g_anisoClamp, uv, lod) : t.SampleLevel(g_anisoWrap, uv, lod);
}

#endif
