// Core stub of track S's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). Each entry declares no passes and logs once. The track's real code lives in its own
// folders; this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void atmosphere(FramePassContext&) { pending("S.atmosphere (track disabled in this build)"); }
void shadowPages(FramePassContext&, const ViewResources&) { pending("S.shadowPages (track disabled in this build)"); }
void froxels(FramePassContext&, const ViewResources&) { pending("S.froxels (track disabled in this build)"); }
void shadowVisibility(FramePassContext&, ViewResources&) { pending("S.shadowVisibility (track disabled in this build)"); }
std::vector<RenderGraph::BandedPass> shadowVisibilityPasses(FramePassContext&, ViewResources&)
{
    pending("S.shadowVisibilityPasses (track disabled in this build)");
    return {};
}
} // namespace unx::render::tracks
