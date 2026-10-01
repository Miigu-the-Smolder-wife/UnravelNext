// Core stub of track R's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). Each entry declares no passes and logs once. The track's real code lives in its own
// folders; this file only lets the other tracks build and run without it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void accelerationStructures(FramePassContext&) { pending("R.accelerationStructures (track disabled in this build)"); }
void surfaceCache(FramePassContext&) {}
void screenTraceInputs(FramePassContext&, ViewResources&) {}
void globalIllumination(FramePassContext&, ViewResources&) { pending("R.globalIllumination (track disabled in this build)"); }
void reflections(FramePassContext&, ViewResources&) { pending("R.reflections (track disabled in this build)"); }
void giScreenIrradiance(FramePassContext&, ViewResources&) {}
void refraction(FramePassContext&, BufferRef, BufferRef, uint32_t) { pending("R.refraction (track disabled in this build)"); }
} // namespace unx::render::tracks
