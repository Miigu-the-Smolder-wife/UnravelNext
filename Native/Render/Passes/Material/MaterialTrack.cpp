// Track entry points of M (material) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void materialResolve(FramePassContext& fc, ViewResources& view)
{
    (void)fc;
    (void)view;
    pending("M.materialResolve");
}
} // namespace unx::render::tracks
