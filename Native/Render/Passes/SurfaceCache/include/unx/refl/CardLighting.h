#pragma once
// The surface cache's lighting on mesh cards (surface_cache.mesh_cards; Passes/SurfaceCache/CardLighting.hlsli states the
// atlases and passes; the structure is Unreal's Lumen scene lighting - LumenSceneLighting.cpp, LumenSceneDirectLighting
// .cpp, LumenRadiosity.cpp): per update the selection (page priority, budgets in 8-texel tiles: direct atlas tiles / 32,
// radiosity / 64), the direct light of the listed tiles (8 lights a tile and the sun, one shadow ray per thread in
// bands), the radiosity of the listed tiles (probes 4 texels apart, 16 rays each, filtered, SH, integrated with a
// 4-frame accumulation) and the final lighting atlas that ray hits read (clReadCards).
#include "unx/refl/CardSet.h"

#include <functional>

namespace unx::render::refl
{
struct CardLightingInputs
{
    CardSet set;
    uint32_t frame = 0;       // the surface cache's update counter (one per update: several a frame while a level loads)
    uint32_t skyVariant = 0;  // 0: atmosphere LUTs, 1: constant sky (the SKY variant of the kernels)
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    bool direct = true, radiosity = true;
    bool shadowRaysOpaque = false;      // surface_cache.shadow_rays_opaque
    float radiosityCap = 40.0f;         // surface_cache.radiosity_max_ray_intensity (exposed units; 0: none)
    float radiosityFrames = 4.0f;       // surface_cache.radiosity_max_frames_accumulated
    // The radiosity rays' re-shoot past a near back face (surface_cache.radiosity_avoid_self_intersections; metres,
    // 0: none) and the least hit distance that reads light (surface_cache.radiosity_min_trace_distance_m).
    float radiositySkipBackFace = 0.05f, radiositySkipTwoSided = 0.01f, radiosityMinTraceDistance = 0.10f;
    uint32_t directFactor = 32;         // surface_cache.direct_update_factor
    uint32_t radiosityFactor = 64;      // surface_cache.radiosity_update_factor
    float depthBias = 0.10f;            // surface_cache.mesh_cards_depth_bias_m
    // The caller's shared declarations and root constants of its ray passes (sky and sun: words 4..14; the ray scene:
    // words 24..31). Words 0..3 and 16..23 are written here.
    std::function<void(PassBuilder&)> declareShared;
    std::function<void(PassContext&, uint32_t*)> sharedConstants;
};

// The lighting's persistent resources in the frame's graph.
struct CardLightingRefs
{
    TextureRef direct, indirect, final, trace, sh[3], frames;
    BufferRef uniformBits, pageLight, frame;
    BufferRef lastUsed;    // per card page: the update a reader of the cards' high levels last read it in
    bool created = false;  // the resources were made this frame (they hold nothing: r.card.clear runs)
};

class CardLighting
{
public:
    explicit CardLighting(Device& device);
    ~CardLighting();
    // Creates (first use, another atlas size or page capacity) and imports the lighting's persistent resources: once a
    // frame, before any pass of the frame names them.
    const CardLightingRefs& begin(FramePassContext& fc, uint32_t atlasSize, uint32_t pageCapacity);
    // r.card.clear when the resources are new or 'restart' (the card set starts from nothing), then r.card.frame: the
    // card frame (CardLayout.hlsli mcFrame) of 'set' as it stands now.
    void recordFrame(FramePassContext& fc, const CardSet& set, bool restart, float depthBias, uint32_t frameWord);
    // One update: selection, direct light, radiosity, final lighting.
    void recordLighting(FramePassContext& fc, const CardLightingInputs& in);
    // The card frame's buffer (readers pass its SRV to clReadCards); invalid before begin() of this frame.
    BufferRef frame(const FramePassContext& fc) const;
    // Every resource a reader of the card frame touches, as shader resources of 'use' (SrvCompute or SrvGraphics).
    void declareRead(const FramePassContext& fc, PassBuilder& b, Use use) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace unx::render::refl
