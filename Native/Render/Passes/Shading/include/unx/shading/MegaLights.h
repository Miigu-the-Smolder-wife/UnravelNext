#pragma once
// Stochastic direct light of the local lights (shading.mega_lights; MegaLights.hlsli states the passes; owner A). The
// structure and default numbers follow Unreal Engine's MegaLights; this replaces the per-pixel loop over every listed
// light with S's shadow slots (128 lights with shadows) by N light samples per downsampled pixel, one shadow ray each,
// and a temporal and spatial filter. Main view only; needs S's froxel lists and the R track's ray scene.
#include "unx/render/Frame.h"

struct ID3D12CommandSignature;

namespace unx::render::shading
{
struct MegaLightsFrame
{
    bool on = false;
    TextureRef samples, keys;                      // the light samples after the trace and the downsampled key
    TextureRef resolvedDiffuse, resolvedSpecular;  // m.ml.shade's outputs (the shading record dispatches it per class)
    TextureRef lighting;                           // m.ml.spatial's result: what the shading kernels add (P[10].y)
    uint32_t factor = 2, count = 4;
    float maxWeight = 20, maxWeightHidden = 5, minSampleWeight = 0.001f;
    // (megaLightsDenoise)
    uint32_t previous = 0, next = 1;
    bool historyValid = false;
    BufferRef sets;        // the persistent tile sets (previous frame's until m.ml.sets.filter rewrites them)
    TextureRef prevDepth;  // the previous frame's view depth (invalid without history)
    float exposureRatio = 1;
};
// m.ml.sample and m.ml.trace of the main view; 'on' is false when the switch is off or the view cannot run it (then the
// shading kernels keep their loop). Creates the textures m.ml.shade writes.
// dispatchSignature: M's one-dispatch command signature (material::dispatchSignature) for the tile list's dispatch.
MegaLightsFrame megaLightsSample(FramePassContext& fc, const ViewResources& view, TextureRef materialWord, bool areaLights, uint32_t ltcSrv,
                                 ID3D12CommandSignature* dispatchSignature);
// m.ml.sets, m.ml.temporal and m.ml.spatial, after the caller's m.ml.shade; sets ml.lighting.
void megaLightsDenoise(FramePassContext& fc, const ViewResources& view, TextureRef materialWord, MegaLightsFrame& ml);
} // namespace unx::render::shading
