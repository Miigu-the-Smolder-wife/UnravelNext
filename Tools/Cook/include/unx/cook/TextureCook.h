#pragma once
// Texture cooking (C1, plan 13.5). Owner: C.
//
// A scene texture becomes the mip chain the renderer stores on the GPU (material::buildMipChain: exact box mips in double,
// coverage-preserving alpha for alpha-tested base colours, slope moments for normal maps), then, where the error bound
// below holds, block-compressed:
//   base colour / emissive RGBA8 sRGB (not alpha-tested)  -> BC7_UNORM_SRGB   (GPU encoder, all 8 modes incl. 3-subset)
//   RGBA8 linear                                         -> BC7_UNORM
//   rough/metal RG8                                      -> BC5_UNORM        (CPU)
//   occlusion R8                                         -> BC4_UNORM        (CPU)
//   normal slope moments RGBA16, HDR RGBA16F, cut-out coverage R8, alpha-tested base colour -> stored as built: block
//   compression cannot keep their definitions (exact slope variance, exact coverage fraction at every level).
//
// Quality definition: every level is encoded from the renderer's own exact level (never from a lower-precision copy), then
// decoded and compared with it byte for byte over its channels; the texture is stored compressed only when every level
// reaches kMinPsnrDb and no texel's channel differs by more than kMaxAbsError / 255. Otherwise it is stored exactly as built
// (the measured PSNR and maximum error are logged either way). A top level whose width or height is not a multiple of 4
// (D3D12 block textures need it) is stored as built.
//
// Cost [expected]: cooking a 2048^2 base colour = mip build (CPU, double) + BC7 encode (GPU, 64-block dispatches: each
// dispatch structurally bounded) + decode check; paid once per texture content, afterwards a disk read (block data =
// 1/4 of RGBA8, 1/2 of RG8). Keys: SHA-256 of texels, size, format, the alpha-test use and the cook code (source hash).
#include "unx/material/TextureSystem.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <memory>
#include <string>

namespace unx::cook
{
constexpr double kMinPsnrDb = 44.0;   // per level, over the stored channels
constexpr uint32_t kMaxAbsError = 24;  // per channel, 8-bit steps

struct TextureCookStats
{
    uint32_t fromMemory = 0, fromDisk = 0, cooked = 0;
    uint32_t compressed = 0;     // cooked chains stored block-compressed
    uint32_t keptExact = 0;      // cooked chains whose format stays uncompressed (moments, HDR, coverage, alpha-tested)
    uint32_t failedBound = 0;    // cooked chains that missed the error bound and were stored as built
    double minPsnrDb = 1e9;      // over the compressed chains' levels
    uint32_t maxAbsError = 0;
    double cookMs = 0;
};

// The stored mip chain of scene texture 'index': kind 0 = the texture, kind 1 = its cut-out coverage (empty when no
// alpha-tested material uses it). Thread-safe. A block-compressed chain has bytesPerTexel = 0 and each level holds its
// blocks in rows of ceil(w / 4) blocks, top to bottom.
render::material::MipChain textureChain(const scene::Scene& scene, uint32_t index, uint32_t kind);

// Disk cache directory; empty = none. Until set, the environment variable UNX_COOK_CACHE (unset: none). Entries live in
// <directory>/textures.
void setTextureCacheDirectory(const std::string& directory);
// Forgets the memory cache (tests that need a fresh process's behaviour).
void clearTextureMemoryCache();
// Counters since the last reset.
TextureCookStats textureCookStats();
void resetTextureCookStats();

// Encodes one RGBA8 / RG8 / R8 image to the block format above and decodes it back (tests, the cook itself). Returns
// false when no encoder is available (no D3D11 device for BC7).
struct BlockResult
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> blocks;   // rows of blocks
    std::vector<uint8_t> decoded;  // same layout as the input
    double psnrDb = 0;
    uint32_t maxAbsError = 0;
};
bool encodeLevel(DXGI_FORMAT source, uint32_t width, uint32_t height, const uint8_t* texels, BlockResult& out);
} // namespace unx::cook
