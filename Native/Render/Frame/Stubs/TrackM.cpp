// Core stub of track M's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). Each entry declares no passes and logs once. The track's real code lives in its own
// folders; this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void prepareScene(FramePassContext&) { pending("M.prepareScene (track disabled in this build)"); }
float autoExposureEv100(TrackState&, Device&, const QualityConfig&, const FrameContext& frame, uint32_t)
{
    pending("M.autoExposureEv100 (track disabled in this build)");
    return frame.mainView.ev100;
}
void materialResolve(FramePassContext&, ViewResources&) { pending("M.materialResolve (track disabled in this build)"); }
void shading(FramePassContext&, ViewResources&) { pending("M.shading (track disabled in this build)"); }
std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext&, ViewResources&)
{
    pending("M.shadingPasses (track disabled in this build)");
    return {};
}
void shadingComposite(FramePassContext&, ViewResources&) { pending("M.shadingComposite (track disabled in this build)"); }
} // namespace unx::render::tracks
