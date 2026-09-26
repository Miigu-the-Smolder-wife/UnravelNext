#pragma once
// M's HDR post chain (Post.cpp; FEATURES_GAME 4 and 6, render A item A4).
#include "unx/render/Frame.h"

namespace unx::render::shading
{
// A post term is on for this view (the main view of a display frame with shading.post_* set).
bool postActive(FramePassContext& fc, const ViewResources& view);
// The exposed-linear RGBA16F target the view is shaded into when the chain is on.
TextureRef postTarget(FramePassContext& fc, const ViewResources& view);
// The lens PSF's tail of 'hdr' at half resolution (energy-normalised pyramid over 'levels' octaves; tests read it).
TextureRef postBloomTail(FramePassContext& fc, TextureRef hdr, uint32_t levels);
// Bloom, vignetting, tone curve, grading, grain, dither and encoding from 'hdr' into view.color.
void postChain(FramePassContext& fc, const ViewResources& view, TextureRef hdr);
} // namespace unx::render::shading
