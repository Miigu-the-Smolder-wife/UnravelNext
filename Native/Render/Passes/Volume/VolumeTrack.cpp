// Track E entry points of Passes/Volume (Tracks.h): particle media for S's froxel integration and the heat haze field.
#include "unx/volume/VolumePass.h"
#include "unx/render/Tracks.h"

#include <memory>

namespace unx::render::tracks
{
namespace
{
volume::VolumePass& volumePass(FramePassContext& fc)
{
    auto& pass = fc.trackState->get<std::unique_ptr<volume::VolumePass>>("volume.pass");
    if (!pass) pass = std::make_unique<volume::VolumePass>(fc.device);
    return *pass;
}
volume::VolumeFrame frameOf(FramePassContext& fc, const ViewResources& view)
{
    volume::VolumeFrame f;
    f.view = &view.view;
    f.frameConstants = view.frameConstants;
    f.mainHeight = fc.frame.mainView.height;
    f.camera[0] = view.view.position.x + fc.frame.worldOrigin[0];  // renderer world; the pass maps it to stream space
    f.camera[1] = view.view.position.y + fc.frame.worldOrigin[1];
    f.camera[2] = view.view.position.z + fc.frame.worldOrigin[2];
    for (int a = 0; a < 3; ++a) f.streamAxes[a] = fc.frame.streamAxes[a];
    f.time = fc.frame.time;
    const FrameResources& r = fc.resources;
    fx::ParticleLighting& l = f.lighting;
    l.vsmPageTable = r.vsmPageTable;
    l.vsmPool = r.vsmPool;
    l.vsmAtlas = r.vsmAtlas;
    l.vsmBlocks = r.vsmBlocks;
    l.vsmSearchBound = r.vsmSearchBound;
    l.vsmConstants = r.vsmConstants;
    l.vsmLocalLights = r.vsmLocalLights;
    l.vsmSlotOfLight = r.vsmSlotOfLight;
    l.vsmLayers = r.vsmLayers;
    l.gi = giSource(r);
    l.transmittanceLut = r.transmittanceLut;
    l.multiScatterLut = r.multiScatterLut;
    return f;
}
} // namespace

TextureRef volumeMedia(FramePassContext& fc, ViewResources& main, BufferRef froxelLights)
{
    if (!fc.trackState || !froxelLights.valid()) return {};
    fx::ParticleSystem* system = fx::findParticles(*fc.trackState);
    if (!system) return {};
    constexpr uint32_t kOutputVolume = 3;  // NV_VOLUME (FX StreamRecords.hlsli FX_OUTPUT_VOLUME): no such program, no media
    if (!system->hasProgramOutput(kOutputVolume)) return {};
    volume::VolumeFrame f = frameOf(fc, main);
    f.froxelLights = froxelLights;
    f.lighting.froxelLights = froxelLights;
    // shading.mega_lights: the grid's sampled local light (S records it before the media; main view's grid)
    f.lighting.localFluence = fc.resources.localFluence;
    f.lighting.localMoment = fc.resources.localMoment;
    f.media = true;
    const volume::VolumeOutput out = volumePass(fc).record(*system, fc.graph, fc.shaders, fc.quality, fc.frame.frameIndex, f);
    main.volumeSlices = out.volumeSlices;
    return out.volumeSlices;
}

void distortion(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState) return;
    fx::ParticleSystem* system = fx::findParticles(*fc.trackState);
    if (!system) return;
    // No distortion program in the stream: no haze field, and M's full-screen distortion pass (0.28 ms at 4K in the train,
    // which has no haze [measured, e6fa5e8]) does not run - its displacement would be 0 everywhere.
    constexpr uint32_t kOutputDistortion = 5;  // NV_DISTORTION (FX StreamRecords.hlsli FX_OUTPUT_DISTORTION)
    if (!system->hasProgramOutput(kOutputDistortion)) return;
    volume::VolumeFrame f = frameOf(fc, view);
    f.haze = true;
    const volume::VolumeOutput out = volumePass(fc).record(*system, fc.graph, fc.shaders, fc.quality, fc.frame.frameIndex, f);
    view.distortionOffset = out.distortionOffset;
    view.distortionDepth = out.distortionDepth;
}
} // namespace unx::render::tracks
