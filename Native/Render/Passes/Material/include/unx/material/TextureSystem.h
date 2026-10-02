#pragma once
// Scene textures on the GPU (M track, ARCHITECTURE 2.2). Every scene texture gets its full mip chain in the form the
// material resolve filters directly with the footprint gradients (SampleGrad, anisotropic):
//   Rgba8Srgb      RGBA8 sRGB. When an alpha-tested material uses it as base colour: colour mips weighted by alpha
//                  (transparent texels never bleed into leaf edges) and alpha mips that keep the fraction of texels
//                  passing the material's cutoff (the coverage of the texture) equal at every level.
//   Rg8Normal      RGBA16_UNORM slope moments (MaterialInternal.hlsli mNormalMoments): mean slope, internal variance
//                  trace and |mean|^2, so the hardware filter yields the exact slope variance of its footprint.
//   Rg8RoughMetal  RG8, Rgba8Linear RGBA8, R8Linear R8, Rgba16Float RGBA16F: box mips of the linear values.
// Mips are box filters of the covered base texels (fractional coverage for odd sizes), computed in double.
// Cut-out coverage (R8): for every base colour texture an alpha-tested material uses, the fraction of base texels whose
// alpha passes the cutoff, box mips of that binary mask; the edge composite filters it over a pixel's footprint to
// weigh a cut-out surface by the part of the pixel it covers (EdgeComposite.hlsl).
// The per-material texture table (MTextureSet, MaterialInternal.hlsli) carries the SRVs.
#include "unx/render/Device.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <string>
#include <vector>

namespace unx::render::material
{
struct TextureSetGpu  // mirror of MTextureSet (MaterialInternal.hlsli), 32 B
{
    uint32_t baseColor, moments, roughMetal, emissive;
    uint32_t occlusion;
    float slopeRange;
    uint32_t flags;          // clamp addressing per texture, gpu::MaterialTextureBit (1 = g_anisoClamp)
    uint32_t coverage;       // cut-out coverage mips of the base colour (alpha-tested materials), gpu::kNone otherwise
};
static_assert(sizeof(TextureSetGpu) == 32);

// CPU form of one texture's mip chain (tests compare against it).
struct MipChain
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    float slopeRange = 0;                   // Rg8Normal: S of the moments encoding
    std::vector<std::vector<uint8_t>> levels;  // tightly packed rows per level
    uint32_t bytesPerTexel = 0;
};

// Builds the mip chain of scene texture 'index' for its uses in 'scene' (base colour of alpha-tested materials gets
// coverage-preserving alpha at that material's cutoff).
MipChain buildMipChain(const scene::Scene& scene, uint32_t index);
// Cut-out coverage mips of scene texture 'index' (R8_UNORM: fraction of base texels with alpha >= the cutoff of the
// alpha-tested materials that use it as base colour); no levels when no alpha-tested material uses it.
MipChain buildCoverageChain(const scene::Scene& scene, uint32_t index);

// Stored-form provider (C1: C's cook, Tools/Cook TextureCook.h): the chain of scene texture 'index' (kind 0) or its cut-out
// coverage (kind 1) as it is uploaded, possibly block-compressed (bytesPerTexel = 0: each level is rows of 4x4 blocks,
// tightly packed) and served from its memory and disk caches. Null (default): buildMipChain / buildCoverageChain.
using ChainProvider = MipChain (*)(const scene::Scene& scene, uint32_t index, uint32_t kind);
void setChainProvider(ChainProvider provider);

class TextureSystem
{
public:
    TextureSystem() = default;
    ~TextureSystem();
    TextureSystem(const TextureSystem&) = delete;
    TextureSystem& operator=(const TextureSystem&) = delete;

    // Uploads textures and the material table when the GPU scene changed (revision). Blocking; load time only.
    void sync(Device& device, const GpuScene& scene);
    uint32_t tableSrv() const { return m_table.srv; }
    // Per scene material, what GpuScene::setMaterialTextures publishes (INTERFACES 6.3 v1.10; formats in
    // Passes/Material/MaterialTextures.hlsli). Valid after sync().
    const std::vector<gpu::MaterialTextures>& published() const { return m_published; }
    bool anyEmissiveTexture() const { return m_anyEmissive; }
    uint32_t textureSrv(uint32_t sceneTexture) const { return m_textures.at(sceneTexture).srv; }
    // Per scene light the SRV of its source texture (scene::Light::sourceTexture: a rect's image), gpu::kNone for a light
    // without one: what GpuScene::setLightSourceTextures publishes. Valid after sync().
    std::vector<uint32_t> lightSourceTextures() const;
    // The same lights' image means (gpu::rgb9e5 of level 0's mean linear colour; 0 for a light without an image). A
    // texture's mean is computed once per upload of the textures.
    std::vector<uint32_t> lightSourceMeans() const;
    uint64_t gpuBytes() const { return m_gpuBytes; }

private:
    struct Resource
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t srv = gpu::kNone;
    };
    void release(Resource& r);
    void clear();

    Device* m_device = nullptr;
    const scene::Scene* m_source = nullptr;
    uint32_t m_revision = UINT32_MAX;
    std::string m_fingerprint;
    std::vector<Resource> m_textures;
    mutable std::vector<uint64_t> m_meanOf;  // per scene texture: bit 32 = known, bits 0..31 its mean (lightSourceMeans)
    std::vector<Resource> m_coverage;  // per scene texture, cut-out coverage (alpha-tested base colours only)
    std::vector<float> m_slopeRange;
    Resource m_table;
    std::vector<gpu::MaterialTextures> m_published;
    bool m_anyEmissive = false;
    uint64_t m_gpuBytes = 0;
};
} // namespace unx::render::material
