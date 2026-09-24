// Track entry point of R (acceleration structures, INTERFACES_KO.md 5.2; ARCHITECTURE 2.12, 4.1 C2).
#include "unx/render/Tracks.h"
#include "unx/rt/RayScene.h"

namespace unx::render::tracks
{
void accelerationStructures(FramePassContext& fc)
{
    if (!fc.scene.source())
    {
        pending("R.accelerationStructures (no scene)");
        return;
    }
    rt::RayScene::get(fc).record(fc);
}
} // namespace unx::render::tracks
