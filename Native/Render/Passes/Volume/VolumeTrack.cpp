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
    f.camera[0] = view.view.position.x;
    f.camera[1] = view.view.position.y;
    f.camera[2] = view.view.position.z;
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
    l.giCache = r.giCache;
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
    volume::VolumeFrame f = frameOf(fc, main);
    f.froxelLights = froxelLights;
    f.lighting.froxelLights = froxelLights;
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
    volume::VolumeFrame f = frameOf(fc, view);
    f.haze = true;
    const volume::VolumeOutput out = volumePass(fc).record(*system, fc.graph, fc.shaders, fc.quality, fc.frame.frameIndex, f);
    view.distortionOffset = out.distortionOffset;
    view.distortionDepth = out.distortionDepth;
}
} // namespace unx::render::tracks
