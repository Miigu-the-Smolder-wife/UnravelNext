// Track entry point of M (shading) (INTERFACES_KO.md 5.2). Signatures fixed by Tracks.h.
#include "unx/shading/Exposure.h"
#include "unx/render/Tracks.h"
#include "unx/shading/ShadingSystem.h"

namespace unx::render::tracks
{
void shading(FramePassContext& fc, ViewResources& view)
{
    shading::shade(fc, view);
}

// The banded lighting group (v1.31): M's edge detection and shading kernels, band by band after S's visibility passes.
// Neither lags: the detection's neighbours are the material resolve's (complete before the group) and S's visibility
// has no screen-space filter, so a band reads only its own rows of the group's products.
// M (A4): automatic exposure (Exposure.cpp).
float autoExposureEv100(TrackState& state, Device& device, const QualityConfig& quality, const FrameContext& frame, uint32_t framesInFlight)
{
    return shading::autoExposureEv100(state, device, quality, frame, framesInFlight);
}

std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view)
{
    return shading::shadingPasses(fc, view);
}

void shadingComposite(FramePassContext& fc, ViewResources& view)
{
    shading::shadingComposite(fc, view);
}
} // namespace unx::render::tracks
