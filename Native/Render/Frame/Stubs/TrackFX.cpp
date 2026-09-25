// Core stub of track FX's entry point (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). It declares no passes and logs once. The track's real code lives in its own folder;
// this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void simulation(FramePassContext&) { pending("FX.simulation (track disabled in this build)"); }
} // namespace unx::render::tracks
