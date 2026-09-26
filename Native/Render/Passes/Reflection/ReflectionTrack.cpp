// Track entry point of R (reflections, INTERFACES_KO.md 5.2; ARCHITECTURE 2.6, 4.1 C4).
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
// Refraction rays (R-W2 water, R-2 glass; FrameServices::traceRefractions): RefractionTrace.hlsl with this frame's
// reflection constants (ReflectionSystem::recordRefraction).
void refraction(FramePassContext& fc, BufferRef jobs, BufferRef results, uint32_t maxJobs)
{
    refl::ReflectionSystem::get(fc).recordRefraction(fc, jobs, results, maxJobs);
}
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
