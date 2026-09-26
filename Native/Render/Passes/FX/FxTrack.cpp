// Track entry points of FX (INTERFACES_KO.md 5.2): the GPU simulation (C0 of ARCHITECTURE 4.1) and the particle render
// pass (G7; request 20260926_FX_particle_render_pass.md).
// The particle module (unx::fx::ParticleSystem, TrackState key "fx.particles") records the ticks its host submitted
// since the last frame; without an attached module or pending ticks the entry declares no passes.
#include "unx/fx/ParticleLayer.h"
#include "unx/fx/Particles.h"
#include "unx/render/Tracks.h"

#include <memory>

namespace unx::render::tracks
{
void simulation(FramePassContext& fc)
{
    if (!fc.trackState) return;
    if (fx::ParticleSystem* particles = fx::findParticles(*fc.trackState)) particles->record(fc);
}

// G7: the particle layer of a view (after reflections, before M's shading, which composites it; ParticleLayer.hlsli).
// The frame time is fc.frame.time on the particle stream's clock (the host renders between the last two committed ticks);
// without a particle module or before its first tick the view's particle fields stay invalid (nothing to composite).
void particles(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState || !view.depth.valid()) return;
    fx::ParticleSystem* system = fx::findParticles(*fc.trackState);
    if (!system) return;
    auto& pass = fc.trackState->get<std::unique_ptr<fx::ParticleLayerPass>>("fx.layer");
    if (!pass) pass = std::make_unique<fx::ParticleLayerPass>(fc.device);
    fx::ParticleLayerFrame frame;
    frame.view = &view.view;
    frame.frameConstants = view.frameConstants;
    frame.depth = view.depth;
    frame.camera[0] = view.view.position.x + fc.frame.worldOrigin[0];
    frame.camera[1] = view.view.position.y + fc.frame.worldOrigin[1];
    frame.camera[2] = view.view.position.z + fc.frame.worldOrigin[2];
    for (int a = 0; a < 3; ++a) frame.streamAxes[a] = fc.frame.streamAxes[a];
    frame.time = fc.frame.time;
    const FrameResources& r = fc.resources;
    frame.lighting.vsmPageTable = r.vsmPageTable;
    frame.lighting.vsmPool = r.vsmPool;
    frame.lighting.vsmAtlas = r.vsmAtlas;
    frame.lighting.vsmBlocks = r.vsmBlocks;
    frame.lighting.vsmSearchBound = r.vsmSearchBound;
    frame.lighting.vsmConstants = r.vsmConstants;
    frame.lighting.vsmLocalLights = r.vsmLocalLights;
    frame.lighting.vsmSlotOfLight = r.vsmSlotOfLight;
    frame.lighting.vsmLayers = r.vsmLayers;
    frame.lighting.giCache = r.giCache;
    frame.lighting.froxelLights = view.froxelLights;
    frame.lighting.fxLights = r.fxLights;
    frame.lighting.airVolume = view.airVolume;
    frame.lighting.transmittanceLut = r.transmittanceLut;
    frame.lighting.multiScatterLut = r.multiScatterLut;
    const fx::ParticleLayerOutput out = pass->record(*system, fc.graph, fc.shaders, fc.frame.frameIndex, frame);
    if (!out.valid) return;
    view.particleLayer = out.layer;
    view.particleDepthRange = out.depthRange;
    view.particleEdges = out.edges;
}
} // namespace unx::render::tracks
