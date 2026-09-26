// Track entry points of W (INTERFACES_KO.md 5.2, Tracks.h): waterGeometry() after the simulation and before V (the ocean
// FFT and the fluid surface into V's triangle streams), water() in M's shading() after the opaque kernel (refraction,
// absorption, reflection of the water pixels). Until the frame carries water (the ocean and fluid sources from the
// World), both declare no passes.
#include "unx/render/Tracks.h"

namespace unx::render::tracks
{
void waterGeometry(FramePassContext&) {}
void water(FramePassContext&, ViewResources&) {}
} // namespace unx::render::tracks
