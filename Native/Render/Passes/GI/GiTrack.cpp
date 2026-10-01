// Track entry point of R (GI, INTERFACES_KO.md 5.2; ARCHITECTURE 2.5, 4.1 C3/C5).
#include "unx/gi/GiSystem.h"
#include "unx/gi/LumenRadianceCache.h"
#include "unx/gi/LumenShortRangeAO.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void globalIllumination(FramePassContext& fc, ViewResources& main)
{
    // Inputs: V's depth, M's G-buffer, R's TLASes (accelerationStructures ran first).
    if (!main.depth.valid() || !main.gbuffer.valid() || !fc.resources.tlasStatic.valid())
    {
        pending("R.globalIllumination (waits for V depth and M G-buffer)");
        return;
    }
    gi::GiSystem::get(fc).record(fc, main, rt::RayScene::get(fc));
    // A's Lumen modules for the final gather (lumen.toml; invalid = off): the short-range AO / bent normal of this frame
    main.shortRangeAO = gi::lumenShortRangeAO(fc, main);
    // and the far-field radiance cache: when the gather did not run it itself (Begin / Update are idempotent per frame),
    // it is updated here from the screen marker alone, with the hit lighting's sources of this frame
    if (gi::LumenRcFrame rc = gi::lumenRadianceCacheBegin(fc, main); rc.on && !rc.updated)
    {
        gi::LumenRcInputs in;
        in.worldCache = fc.resources.giCache;
        in.surfaceCache = fc.resources.surfaceCache;
        in.experiment = gi::GiSystem::get(fc).settings().experimentDisable;
        gi::lumenRadianceCacheUpdate(fc, main, rt::RayScene::get(fc), in, rc);
    }
}

void giScreenIrradiance(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState) return;
    if (gi::GiSystem* gi = gi::GiSystem::find(*fc.trackState)) gi->recordSecondaryScreen(fc, view);
}
} // namespace unx::render::tracks
