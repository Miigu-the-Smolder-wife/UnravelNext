// Track entry point of R (reflections, INTERFACES_KO.md 5.2; ARCHITECTURE 2.6, 4.1 C4).
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
// R-W2 / R-2 placeholder (core's service, 2026-09-27): render B replaces this with the refraction tracer.
void refraction(FramePassContext&, BufferRef, BufferRef, uint32_t) { pending("R.refraction (tracer not implemented yet)"); }
void reflections(FramePassContext& fc, ViewResources& main)
{
    // Inputs: V's depth, M's G-buffer (and reflection lobe tiles when M provides them), R's GI of this frame.
    if (!main.depth.valid() || !main.gbuffer.valid() || !main.screenProbes.valid() || !fc.resources.giCache.valid())
    {
        pending("R.reflections (waits for V depth, M G-buffer and R GI)");
        return;
    }
    refl::ReflectionSystem::get(fc).record(fc, main, rt::RayScene::get(fc));
}
} // namespace unx::render::tracks
