// Core stub of track M's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). Each entry declares no passes and logs once. The track's real code lives in its own
// folders; this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void materialResolve(FramePassContext&, ViewResources&) { pending("M.materialResolve (track disabled in this build)"); }
void shading(FramePassContext&, ViewResources&) { pending("M.shading (track disabled in this build)"); }
} // namespace unx::render::tracks
