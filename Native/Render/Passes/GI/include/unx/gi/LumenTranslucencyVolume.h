#pragma once
// The Lumen translucency volume (lumen.translucency_volume; Passes/GI/LumenTranslucencyVolume.hlsli states the structure):
// indirect light for everything that is not an opaque surface of the view - air and fog, particles, water, glass.
// Per frame of the main view, inside the final gather's record:
//   gi::lumenTranslucencyVolumeMark(fc, main, rc);                 // between the radiance cache's Begin and Update
//   gi::lumenTranslucencyVolume(fc, main, rays, inputs, rc);       // after the cache's Update: trace, filter, integrate
// The second call publishes FrameResources::translucencyGi (the two volume textures, for the readers' declarations)
// and translucencyGiParams (the raw SRV readers pass to ltvIrradiance / ltvRadiance).
#include "unx/gi/LumenRadianceCache.h"
#include "unx/render/Frame.h"

namespace unx::render::gi
{
struct LumenTvInputs
{
    SurfaceCacheCardRefs cards;              // the mesh-card surface cache (the rays' hit lighting); invalid: hits are black
    float3 skyRadiance{}, sunIlluminance{};  // constants of the SKY1 variant (no atmosphere tables in the frame)
};
void lumenTranslucencyVolumeMark(FramePassContext& fc, const ViewResources& main, const LumenRcFrame& rc);
// The volume the previous frame published, in this frame's graph, for a pass recorded before this frame's update (the
// textures are imported once: the update's history read uses the same references). params 0xFFFFFFFF: none - the module
// is off, nothing was published last frame, or this frame cuts.
struct LumenTvPrevious
{
    uint32_t params = 0xFFFFFFFFu;
    TextureRef ambient, directional;
};
LumenTvPrevious lumenTranslucencyVolumePrevious(FramePassContext& fc, const ViewResources& main);
void lumenTranslucencyVolume(FramePassContext& fc, const ViewResources& main, rt::RayScene& rays, const LumenTvInputs& inputs, const LumenRcFrame& rc);
} // namespace unx::render::gi
