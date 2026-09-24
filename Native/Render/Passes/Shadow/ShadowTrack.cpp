// Track entry points of S (shadow) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void shadowPages(FramePassContext& fc, const ViewResources& main)
{
    (void)fc;
    (void)main;
    pending("S.shadowPages");
}

void shadowVisibility(FramePassContext& fc, ViewResources& view)
{
    (void)fc;
    (void)view;
    pending("S.shadowVisibility");
}
} // namespace unx::render::tracks
