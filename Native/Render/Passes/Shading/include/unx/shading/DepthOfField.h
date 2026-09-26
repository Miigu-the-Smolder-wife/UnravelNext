#pragma once
// M's lens integral (DepthOfField.cpp; FEATURES_GAME 4.1, COVERAGE 14.12 (2c), render A item A5): depth of field.
#include "unx/render/Frame.h"

namespace unx::render::shading
{
// The lens is open for this view: the main view with FrameContext::lensAperture > 0, a focus distance and band A depth.
bool depthOfFieldActive(FramePassContext& fc, const ViewResources& view);
// The aperture integral of 'src' (exposed linear radiance) into 'dst' (same size, a float format) over the view's depth.
// Returns its statistics buffer (raw, 2 words: pixels gathered, pixels whose radius was clamped to 512 px).
// Tests: the per-pixel radius (R32F) and the per-tile octave maxima and reach (raw, 8 floats per 32 px tile).
struct DepthOfFieldProducts
{
    TextureRef coc;
    BufferRef maxima, reach;
    TextureRef colour[8], shape[8];  // the octave pyramid's levels 1..7
};
BufferRef depthOfField(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, DepthOfFieldProducts* products = nullptr);
} // namespace unx::render::shading
