// Track entry points of S (atmosphere) (INTERFACES_KO.md 5.2). froxels() is in Passes/Shadow (it reads the VSM).
#include "AtmosphereSystem.h"

#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void atmosphere(FramePassContext& fc)
{
    atmosphere::record(fc);
}
} // namespace unx::render::tracks
