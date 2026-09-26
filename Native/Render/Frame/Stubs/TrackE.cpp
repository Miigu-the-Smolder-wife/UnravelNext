// Core stub of track E's entry points (Tracks.h), compiled only when that track is disabled in this build
// (cmake/Tracks.cmake, UNX_TRACKS). It declares no passes and logs once. The track's real code lives in its own folders.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
TextureRef volumeMedia(FramePassContext&, ViewResources&, BufferRef) { pending("E.volumeMedia (track disabled in this build)"); return {}; }
void distortion(FramePassContext&, ViewResources&) { pending("E.distortion (track disabled in this build)"); }
uint32_t debugBegin(FramePassContext&) { pending("E.debugBegin (track disabled in this build)"); return 0xFFFFFFFFu; }
void debugOverlay(FramePassContext&, ViewResources&) { pending("E.debugOverlay (track disabled in this build)"); }
void decals(FramePassContext&, ViewResources&) { pending("E.decals (track disabled in this build)"); }
void surfaceState(FramePassContext&) { pending("E.surfaceState (track disabled in this build)"); }
void hair(FramePassContext&, ViewResources&) { pending("E.hair (track disabled in this build)"); }
float viewModelPrepare(TrackState&, GpuScene&, const QualityConfig&, const FrameContext&) { pending("E.viewModelPrepare (track disabled in this build)"); return 1.0f; }
} // namespace unx::render::tracks
