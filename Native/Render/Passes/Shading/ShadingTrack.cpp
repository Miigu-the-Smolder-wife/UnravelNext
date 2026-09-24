// Track entry points of M (shading) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void shading(FramePassContext& fc, ViewResources& view)
{
    (void)fc;
    (void)view;
    pending("M.shading");
}
} // namespace unx::render::tracks
