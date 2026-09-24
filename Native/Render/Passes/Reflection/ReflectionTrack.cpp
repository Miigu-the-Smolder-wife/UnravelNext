// Track entry points of R (reflection) (INTERFACES_KO.md 5.2). Owned by that track: replace the bodies with the real
// passes; keep the signatures (Tracks.h). Until then each entry declares no passes and logs once.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void reflections(FramePassContext& fc, ViewResources& main)
{
    (void)fc;
    (void)main;
    pending("R.reflections");
}
} // namespace unx::render::tracks
