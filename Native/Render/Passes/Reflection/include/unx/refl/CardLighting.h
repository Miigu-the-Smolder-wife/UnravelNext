#pragma once
// The surface cache's lighting on mesh cards (surface_cache.mesh_cards; S2. Passes/SurfaceCache/CardLighting.hlsli states
// the atlases and passes; Docs/Status/MESH_CARDS_INTERFACE_KO.md section 9 the structure it follows): per frame the
// update selection (page priority, budgets in 8-texel tiles: direct atlas tiles / 32, radiosity / 64), the direct light
// of the listed tiles (8 lights a tile and the sun, one shadow ray per thread in bands), the radiosity of the listed
// tiles (probes 4 texels apart, 16 rays each, filtered, SH, integrated with a 4-frame accumulation) and the final
// lighting atlas that ray hits read (clReadCards).
#include "unx/refl/CardSet.h"

#include <functional>

namespace unx::render::refl
{
struct CardLightingInputs
{
    CardSet set;
    uint32_t frame = 0;       // the surface cache's frame word
    uint32_t skyVariant = 0;  // 0: atmosphere LUTs, 1: constant sky (the SKY variant of the caller's kernels)
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    bool direct = true, radiosity = true;
    bool shadowRaysOpaque = false;      // surface_cache.shadow_rays_opaque
    float radiosityCap = 40.0f;         // surface_cache.radiosity_max_ray_intensity (exposed units; 0: none)
    float radiosityFrames = 4.0f;       // surface_cache.radiosity_max_frames_accumulated
    uint32_t directFactor = 32;         // surface_cache.direct_update_factor
    uint32_t radiosityFactor = 64;      // surface_cache.radiosity_update_factor
    // The caller's shared declarations and root constants of its ray passes (sky and sun: words 4..14; the ray scene:
    // words 24..31). Words 0..3 and 16..23 are written here.
    std::function<void(PassBuilder&)> declareShared;
    std::function<void(PassContext&, uint32_t*)> sharedConstants;
};

class CardLighting
{
public:
    explicit CardLighting(Device& device);
    ~CardLighting();
    // The card frame's buffer of this frame (imported once a frame): passes recorded before record() name its SRV.
    BufferRef prepare(FramePassContext& fc);
    // Records the frame's lighting passes. After it, declareRead() serves this frame's readers.
    void record(FramePassContext& fc, const CardLightingInputs& in);
    // The card frame (CardLayout.hlsli mcFrame): readers pass its SRV to clReadCards. Invalid when record() did not run
    // this frame.
    BufferRef frame(const FramePassContext& fc) const;
    // Every resource a reader of the card frame touches, as shader resources of 'use' (SrvCompute or SrvGraphics).
    void declareRead(const FramePassContext& fc, PassBuilder& b, Use use) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace unx::render::refl
