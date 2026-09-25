// Track entry point of FX (GPU simulation: particles first, INTERFACES_KO.md 5.2; C0 of ARCHITECTURE 4.1).
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
} // namespace unx::render::tracks
