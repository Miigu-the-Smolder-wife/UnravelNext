// Track entry point of M (material resolve) (INTERFACES_KO.md 5.2). Signatures fixed by Tracks.h.
#include "unx/material/MaterialSystem.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void materialResolve(FramePassContext& fc, ViewResources& view)
{
    material::resolve(fc, view);
}
} // namespace unx::render::tracks
