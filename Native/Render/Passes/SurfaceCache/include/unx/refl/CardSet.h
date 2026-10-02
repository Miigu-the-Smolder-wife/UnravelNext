#pragma once
// The mesh card set as the card lighting reads it (the shader side is Passes/SurfaceCache/CardLayout.hlsli); produced by
// SurfaceCacheCards (unx/refl/SurfaceCacheCards.h: generation, capture, residency).
#include "unx/render/Frame.h"

#include <memory>

namespace unx::render::refl
{
// The rules the frame's ray hits share (CardLayout.hlsli words 28..39; LumenHitIndirect.hlsli lhiRules): the readers'
// root constants are full, so what every hit kernel needs travels with the card frame.
struct CardHitRules
{
    float farStart = 0;                 // lumen.radiance_cache_far_field: the mesh cards' end, m (0: no far field)
    float skyLeakingInvDistance = 0.1f; // 1 / lumen.skylight_leaking_full_distance_m
    float distantScreenTrace = 0;       // reflection.lumen_distant_screen_traces: their length past the rays' end, m (0: none)
    float distantSlopeTolerance = 2.0f; // reflection.lumen_distant_screen_trace_depth_threshold
    float skyLeaking[3] = { 0, 0, 0 };  // lumen.skylight_leaking x lumen.skylight_leaking_tint (0: none)
    float skyLeakingReflection = 0.25f; // lumen.skylight_leaking_reflection_average_albedo
    float distantStepOffsetBias = 0;    // reflection.lumen_distant_screen_trace_step_offset_bias
};

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
    CardHitRules hitRules;
};
} // namespace unx::render::refl
