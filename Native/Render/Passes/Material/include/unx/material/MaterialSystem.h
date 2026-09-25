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
    Count = 4,
};
constexpr uint32_t kShadeClassCount = (uint32_t)ShadeClass::Count;
constexpr uint32_t kTile = 8;

// Per-view products of the resolve that only M reads (MaterialInternal.hlsli formats).
struct ResolveOutputs
{
    TextureRef materialWord;  // R32_UINT: material 16 | metallic 8
    TextureRef emissive;      // RGBA16F, only when the scene has emissive textures (else invalid)
    BufferRef tiles;          // raw: per class, tileCount entries (x | y << 16)
    BufferRef tileArgs;       // raw: per class D3D12_DISPATCH_ARGUMENTS (12 B), x = tile count
    BufferRef tileFlags;      // raw: one uint per tile, zeroed by the resolve (edge-tile de-duplication)
    uint32_t tilesX = 0, tilesY = 0;
    uint32_t textureTableSrv = 0;
};

// Tests: when fc.state<ResolveDebug>("M.resolveDebug").buffer is valid, the resolve runs its DEBUG variant and writes
// 3 float4 per pixel there (Resolve.hlsl header). The buffer is a graph resource of the frame being recorded.
struct ResolveDebug
{
    BufferRef buffer;
};

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
