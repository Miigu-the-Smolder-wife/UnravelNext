#pragma once
// The mesh card set as the card lighting reads it (the shader side is Passes/SurfaceCache/CardLayout.hlsli); produced by
// SurfaceCacheCards (unx/refl/SurfaceCacheCards.h: generation, capture, residency).
#include "unx/render/Frame.h"

#include <memory>

namespace unx::render::refl
{
struct CardSet
{
    bool valid = false;
    BufferRef instanceMap, meshCards, cards, cardPages, pageTable;  // raw
    TextureRef depth, albedo, normal, emissive;                     // atlasSize^2
    uint32_t atlasSize = 0;
    uint32_t cardPageCapacity = 0;  // S2's per-page state is sized by it
    uint32_t cardPageCount = 0;     // pages in use (indices below it)
    uint32_t instances = 0;         // entries of instanceMap
    uint64_t generation = 0;        // changes when the set was rebuilt from nothing (S2's lighting starts over)
    // The readers' feedback (CardLighting.hlsli clFeedback; surface_cache.feedback): the table the frame's hits write,
    // the dither word (tile jitter x | y << 8 | tile mask << 16) and the resolution level bias. Invalid: none.
    BufferRef feedback;
    uint32_t feedbackDither = 0;
    float feedbackBias = -0.5f;
    bool lightingFeedback = true;   // surface_cache.lighting_feedback: pages the high levels' readers read are relit first
    // The translucency volume hits without cards take their indirect light from while the surface cache's own passes
    // run: the previous frame's (LumenHitIndirect.hlsli). hitVolumeParams 0xFFFFFFFF: none.
    uint32_t hitVolumeParams = 0xFFFFFFFFu;
    TextureRef hitVolumeAmbient, hitVolumeDirectional;
};
} // namespace unx::render::refl
