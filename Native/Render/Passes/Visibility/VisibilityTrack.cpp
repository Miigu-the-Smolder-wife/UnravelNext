// Track entry points of V (visibility) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void visibility(FramePassContext& fc, ViewResources& view)
{
    (void)fc;
    (void)view;
    pending("V.visibility");
}

void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& request)
{
    (void)fc;
    (void)request;
    pending("V.rasterizeDepth");
}
} // namespace unx::render::tracks
