// Core stub of track W's entry point (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). It declares no passes and logs once. The track's real code lives in its own folder;
// this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void waterGeometry(FramePassContext&) { pending("W.waterGeometry (track disabled in this build)"); }
void water(FramePassContext&, ViewResources&) { pending("W.water (track disabled in this build)"); }
} // namespace unx::render::tracks
