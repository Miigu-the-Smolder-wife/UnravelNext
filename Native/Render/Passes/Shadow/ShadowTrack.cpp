// Track entry points of S (shadow) (INTERFACES_KO.md 5.2).
#include "VsmSystem.h"

#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void shadowPages(FramePassContext& fc, const ViewResources& main)
{
    shadow::recordPages(fc, main);
}

void shadowVisibility(FramePassContext& fc, ViewResources& view)
{
    shadow::recordVisibility(fc, view);
}
} // namespace unx::render::tracks
