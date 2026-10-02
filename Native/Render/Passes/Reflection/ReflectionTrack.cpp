// Track entry point of R (reflections, INTERFACES_KO.md 5.2; ARCHITECTURE 2.6, 4.1 C4).
#include "unx/refl/ReflectionSystem.h"
#include "unx/refl/SurfaceCacheCards.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
// Refraction rays (R-W2 water, R-2 glass; FrameServices::traceRefractions): RefractionTrace.hlsl with this frame's
// reflection constants (ReflectionSystem::recordRefraction).
void refraction(FramePassContext& fc, BufferRef jobs, BufferRef results, uint32_t maxJobs)
{
    refl::ReflectionSystem::get(fc).recordRefraction(fc, jobs, results, maxJobs);
}
// (R: the ray hits of GI and reflections read the surface cache. The mesh-card cache is updated here, before both;
// the hashed-cell cache's buffer is only published - its passes run with the reflections)
void surfaceCache(FramePassContext& fc, ViewResources& main)
{
    if (!fc.trackState) return;
    refl::ReflectionSystem& reflections = refl::ReflectionSystem::get(fc);
    if (fc.resources.tlasStatic.valid())
    {
        refl::SurfaceCacheCards& cards = refl::SurfaceCacheCards::get(fc);
        cards.setConstantSky(reflections.constantSkyRadiance(), reflections.constantSunIlluminance());
        cards.record(fc, main, rt::RayScene::get(fc));
    }
    fc.resources.surfaceCache = reflections.surfaceCacheBuffer(fc);
}
// (R, gi.lumen_screen_traces: the probes' rays use the shared screen trace; only when GI asks for it)
void screenTraceInputs(FramePassContext& fc, ViewResources& main)
{
    const QualityConfig& q = fc.quality;
    if (!fc.trackState || !main.depth.valid() || !q.has("gi.lumen") || !q.boolean("gi.lumen") || !q.has("gi.lumen_screen_traces") ||
        !q.boolean("gi.lumen_screen_traces"))
        return;
    fc.resources.screenTraceHzb = refl::ReflectionSystem::get(fc).screenTraceInputs(fc, main).hzb;
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
