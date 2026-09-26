// Track E entry points of Passes/Debug (Tracks.h). Interim: debug drawing off until DebugDraw.cpp lands (same session).
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
uint32_t debugBegin(FramePassContext&) { return 0xFFFFFFFFu; }
void debugOverlay(FramePassContext&, ViewResources&) {}
} // namespace unx::render::tracks
