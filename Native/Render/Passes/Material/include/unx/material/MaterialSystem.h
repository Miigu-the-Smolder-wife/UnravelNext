#pragma once
// M track, material resolve (ARCHITECTURE 2.2, 2.11; INTERFACES 5.2, 5.5.1, 7.2). One kernel per 8 x 8 tile turns the
// vis buffer into the G-buffer (8 B) and M's internal per-pixel material word, classifies the tile by shade class for
// the shading kernels' indirect dispatches, and writes the reflection lobe tiles R reads (v1.3).
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::material
{
// Mirror of M_CLASS_* (MaterialInternal.hlsli): one tile list and one shading kernel per class.
enum class ShadeClass : uint32_t
{
    Sky = 0,
    Opaque = 1,      // Standard + Foliage (INTERFACES 8.1)
    Subsurface = 2,
    Water = 3,
    Layered = 4,     // A9: Standard materials with a clearcoat, ShadeOpaque LAYERED=1
    Sheen = 5,       // A9: Standard materials with a sheen, ShadeOpaque LAYERED=2
    Count = 6,
};
constexpr uint32_t kShadeClassCount = (uint32_t)ShadeClass::Count;
constexpr uint32_t kTile = 8;

// First row of screen band b of 'bands' over 'height' rows: the render graph's split (passBand, v1.31; b = bands gives the
// height). Resolve.hlsl and ShadeBegin.hlsl repeat the same formula on the GPU.
inline uint32_t bandRow(uint32_t height, uint32_t bands, uint32_t b) { return b >= bands ? height : passBand(height, bands, b).y0; }

// Per-view products of the resolve that only M reads (MaterialInternal.hlsli formats). The class tile lists are split by
// screen band (passBandCount of the view), so a banded shading pass dispatches its band's tiles only.
struct ResolveOutputs
{
    TextureRef materialWord;  // R32_UINT: material 16 | metallic 8
    TextureRef emissive;      // RGBA16F, only when the scene has emissive textures (else invalid)
    TextureRef anisoWord;     // R32_UINT, only when the scene has anisotropic materials: Aniso.hlsli's frame word (else invalid)
    BufferRef tiles;          // raw: class c, band b at entry c * tileCount + tilesX * (bandRow(b) / 8), (x | y << 16)
    BufferRef tileArgs;       // raw: D3D12_DISPATCH_ARGUMENTS (12 B) per (class, band) at 12 (c * bands + b), x = tile
                              // count; then one uint per class, the class's clamped total (ShadeBegin, statistics)
    uint32_t tilesX = 0, tilesY = 0;
    uint32_t bands = 1, height = 0;
    uint32_t textureTableSrv = 0;
    uint32_t firstTile(uint32_t shadeClass, uint32_t band) const { return shadeClass * tilesX * tilesY + tilesX * (bandRow(height, bands, band) / 8); }
    uint32_t argsOffset(uint32_t shadeClass, uint32_t band) const { return 12 * (shadeClass * bands + band); }
    uint32_t totalsOffset() const { return 12 * kShadeClassCount * bands; }
};

// Tests: when fc.state<ResolveDebug>("M.resolveDebug").buffer is valid, the resolve runs its DEBUG variant and writes
// 3 float4 per pixel there (Resolve.hlsl header). The buffer is a graph resource of the frame being recorded.
struct ResolveDebug
{
    BufferRef buffer;
};

// Scene preparation (INTERFACES 5.2 v1.10, before any view's frame constants): uploads the scene's textures when the
// GPU scene changed and publishes them in gpu::Material (GpuScene::setMaterialTextures), so V's alpha test, R's hit
// shading and M's resolve read the same textures.
void prepareScene(FramePassContext& fc);

// Records the resolve of 'view' (needs view.visId and view.visibleClusters). Writes view.gbuffer and
// view.reflectionLobeTiles; the internal outputs are kept for shading of the same view in this frame.
void resolve(FramePassContext& fc, ViewResources& view);
// The resolve outputs of 'view' in this frame (fails when resolve() did not run for it).
const ResolveOutputs& resolveOutputs(FramePassContext& fc, const ViewResources& view);
// Command signature of one D3D12_DISPATCH_ARGUMENTS (ExecuteIndirect over the tile args).
ID3D12CommandSignature* dispatchSignature(FramePassContext& fc);
// The texture table SRV (TextureSystem synchronised with the scene). Tests' stand-in visibility uses it too.
uint32_t textureTable(FramePassContext& fc);
} // namespace unx::render::material
