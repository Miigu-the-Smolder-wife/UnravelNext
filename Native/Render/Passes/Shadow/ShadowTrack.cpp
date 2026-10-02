// Track entry points of S (shadow, froxels) (INTERFACES_KO.md 5.2).
#include "FroxelSystem.h"
#include "VsmSystem.h"

#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void shadowPages(FramePassContext& fc, const ViewResources& main)
{
    shadow::recordPages(fc, main);
}

void froxels(FramePassContext& fc, const ViewResources& main)
{
    shadow::recordFroxels(fc, main);
}

uint32_t fogParams(FramePassContext& fc, const ViewDesc& view)
{
    return shadow::fogPrepare(fc, view);
}

uint32_t fogParamsSecondary(FramePassContext& fc, const ViewDesc& view, uint64_t key)
{
    return shadow::fogPrepareSecondary(fc, view, key);
}

void shadowVisibility(FramePassContext& fc, ViewResources& view)
{
    shadow::recordVisibility(fc, view);
}
} // namespace unx::render::tracks
