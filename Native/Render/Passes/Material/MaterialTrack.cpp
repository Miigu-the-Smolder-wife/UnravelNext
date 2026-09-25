// Track entry point of M (material resolve) (INTERFACES_KO.md 5.2). Signatures fixed by Tracks.h.
#include "unx/material/MaterialSystem.h"
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
// Called by FrameRenderer::record before any view's frame constants (INTERFACES 5.2 v1.10).
void prepareScene(FramePassContext& fc)
{
    material::prepareScene(fc);
}

void materialResolve(FramePassContext& fc, ViewResources& view)
{
    material::resolve(fc, view);
}
} // namespace unx::render::tracks
