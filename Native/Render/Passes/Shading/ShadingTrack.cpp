// Track entry point of M (shading) (INTERFACES_KO.md 5.2). Signatures fixed by Tracks.h.
#include "unx/render/Tracks.h"
#include "unx/shading/ShadingSystem.h"

namespace unx::render::tracks
{
void shading(FramePassContext& fc, ViewResources& view)
{
    shading::shade(fc, view);
}
} // namespace unx::render::tracks
