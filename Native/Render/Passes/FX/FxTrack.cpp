// Track entry points of FX (INTERFACES_KO.md 5.2): the GPU simulation (C0 of ARCHITECTURE 4.1) and the particle render
// pass (G7; request 20260926_FX_particle_render_pass.md).
// The particle module (unx::fx::ParticleSystem, TrackState key "fx.particles") records the ticks its host submitted
// since the last frame; without an attached module or pending ticks the entry declares no passes.
#include "unx/fx/ParticleLayer.h"
#include "unx/fx/Particles.h"
#include "unx/fx/SpriteLooks.h"
#include "unx/render/GpuScene.h"
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
    frame.soft = fc.quality.has("fx.particles.soft") && fc.quality.boolean("fx.particles.soft");
    frame.nearFade = fc.quality.has("fx.particles.near_fade") && fc.quality.boolean("fx.particles.near_fade");
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
    frame.lighting.gi = giSource(r);
    frame.lighting.froxelLights = view.froxelLights;
    frame.lighting.fxLights = r.fxLights;
    frame.lighting.airVolume = view.airVolume;
    frame.lighting.fogVolume = view.view.kind == gpu::ViewKind::Main ? r.fogVolume : view.fogVolume;
    frame.lighting.transmittanceLut = r.transmittanceLut;
    frame.lighting.multiScatterLut = r.multiScatterLut;
    // (the sampled local light of this view's froxel grid: the main view's in the frame resources, another view's in its own)
    const bool mainView = view.view.kind == gpu::ViewKind::Main;
    frame.lighting.localFluence = mainView ? r.localFluence : view.localFluence;
    frame.lighting.localMoment = mainView ? r.localMoment : view.localMoment;
    // the sprite looks with the scene's textures, and for the main view under the temporal upscale the previous
    // unjittered view (the sprites' motion: Upscale.cpp's layer motion)
    frame.looks = &fx::spriteLooks(*fc.trackState);
    frame.source = fc.scene.source();
    frame.textureSrvs = fc.scene.textureSrvs();
    if (view.view.kind == gpu::ViewKind::Main && fc.frame.upscale.outputWidth != 0 && !fc.frame.upscale.reset)
    {
        frame.prevViewProj = fc.frame.upscale.prevViewProj;
        frame.motion = true;
    }
    const fx::ParticleLayerOutput out = pass->record(*system, fc.graph, fc.shaders, fc.frame.frameIndex, frame);
    if (!out.valid) return;
    view.particleLayer = out.layer;
    view.particleMotion = frame.motion ? out.motion : TextureRef{};
    view.particleDepthRange = out.depthRange;
    view.particleEdges = out.edges;
}
} // namespace unx::render::tracks
