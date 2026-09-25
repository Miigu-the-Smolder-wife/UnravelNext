// Track entry point of FX (GPU simulation: particles first, INTERFACES_KO.md 5.2; C0 of ARCHITECTURE 4.1). Core's empty
// implementation; the FX session fills it.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void simulation(FramePassContext&) { pending("FX.simulation"); }
} // namespace unx::render::tracks
