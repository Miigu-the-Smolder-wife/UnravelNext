#pragma once
// Far-field radiance cache for the screen-probe final gather (lumen.radiance_cache; LumenRadianceCache.hlsli states the
// structure; owner A, for R's gi.lumen). Follows the structure and defaults of Unreal's Lumen radiance cache.
// Use by a consumer (R's gather), per frame of the main view:
//   LumenRcFrame rc = gi::lumenRadianceCacheBegin(fc, main);          // clears the indirection, marks the screen
//   ... the consumer's own marking pass may write rc.indirection (UAV; LumenRadianceCacheMark.hlsli lrcMark) ...
//   gi::lumenRadianceCacheUpdate(fc, main, rays, inputs, rc);         // allocation, probe rays, filter
//   ... the consumer's ray pass reads rc.params (raw SRV), rc.indirection and rc.atlas (SRVs) with lrcCoverageChecked /
//       lrcSample (LumenRadianceCache.hlsli): rays stop at coverage.minTraceDistance and a miss takes lrcSample ...
// Both calls are idempotent within a frame (a second Begin returns the frame's record; a second Update does nothing), so
// the track's own call after R's record (GiTrack.cpp) only runs the cache when no consumer did.
#include "unx/render/Frame.h"

namespace unx::render::rt
{
class RayScene;
}

namespace unx::render::gi
{
// What the probe rays' hit lighting takes (the same sources as the screen probes' rays, Lumen/LgTrace.hlsl).
struct LumenRcInputs
{
    BufferRef worldCache;      // R's GI cache (hits without a lit surface-cache cell read it); invalid: none
    SurfaceCacheCardRefs cards;  // the mesh-card surface cache (FrameResources::cards); invalid: none
    float3 skyRadiance{}, sunIlluminance{};  // constants of the SKY1 variant (no atmosphere LUTs in the frame)
    uint32_t experiment = 0;   // gi.experiment_disable bits the hit lighting honours (8, 16, 128)
};
struct LumenRcFrame
{
    bool on = false;
    TextureRef indirection;    // Texture3D R32_UINT (grid x clipmaps, grid, grid)
    TextureRef atlas;          // R11G11B10F, (atlas probes x (probe resolution + 2))^2: nits x LRC_RADIANCE_SCALE
    TextureRef depth;          // R16_UINT, (atlas probes x probe resolution)^2: lrcEncodeDepth
    uint32_t params = 0xFFFFFFFFu;  // raw SRV of this frame's LrcParams
    bool updated = false;
};
LumenRcFrame lumenRadianceCacheBegin(FramePassContext& fc, const ViewResources& main);
void lumenRadianceCacheUpdate(FramePassContext& fc, const ViewResources& main, rt::RayScene& rays, const LumenRcInputs& inputs, LumenRcFrame& frame);
} // namespace unx::render::gi
