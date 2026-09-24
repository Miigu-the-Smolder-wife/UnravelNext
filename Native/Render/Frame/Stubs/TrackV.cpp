// Core stub of track V's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). Each entry declares no passes and logs once. The track's real code lives in its own
// folders; this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void visibility(FramePassContext&, ViewResources&) { pending("V.visibility (track disabled in this build)"); }
void rasterizeDepth(FramePassContext&, const DepthRasterRequest&) { pending("V.rasterizeDepth (track disabled in this build)"); }
} // namespace unx::render::tracks
