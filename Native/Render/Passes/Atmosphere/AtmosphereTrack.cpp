// Track entry points of S (atmosphere) (INTERFACES_KO.md 5.2).
#include "AtmosphereSystem.h"

#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void atmosphere(FramePassContext& fc)
{
    atmosphere::record(fc);
}

void froxels(FramePassContext& fc, const ViewResources& main)
{
    (void)fc;
    (void)main;
    pending("S.froxels");
}
} // namespace unx::render::tracks
