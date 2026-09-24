// Track entry points of S (atmosphere) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void atmosphere(FramePassContext& fc)
{
    (void)fc;
    pending("S.atmosphere");
}

void froxels(FramePassContext& fc, const ViewResources& main)
{
    (void)fc;
    (void)main;
    pending("S.froxels");
}
} // namespace unx::render::tracks
