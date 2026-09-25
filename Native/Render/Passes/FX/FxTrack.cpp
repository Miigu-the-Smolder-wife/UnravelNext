// Track entry points of FX (INTERFACES_KO.md 5.2): the GPU simulation (C0 of ARCHITECTURE 4.1) and the particle render
// pass (G7; request 20260926_FX_particle_render_pass.md).
// The particle module (unx::fx::ParticleSystem, TrackState key "fx.particles") records the ticks its host submitted
// since the last frame; without an attached module or pending ticks the entry declares no passes.
#include "unx/fx/Particles.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void simulation(FramePassContext& fc)
{
    if (!fc.trackState) return;
    if (fx::ParticleSystem* particles = fx::findParticles(*fc.trackState)) particles->record(fc);
}

// G7: the particle layer of a view (after reflections, before M's shading, which composites it). Declares no passes until
// the render pass lands (the view's particle fields stay invalid: nothing to composite).
void particles(FramePassContext& fc, ViewResources& view)
{
    (void)fc;
    (void)view;
}
} // namespace unx::render::tracks
