#pragma once
// M's temporal upscale (Upscale.cpp; output.render_height_max, FrameContext::Upscale): the main view renders below the
// output resolution with a sub-pixel jitter and this pass reconstructs the output resolution before the post chain.
#include "unx/render/Frame.h"

namespace unx::render::shading
{
// The main view renders at the internal resolution this frame (FrameContext::upscale.outputWidth != 0).
bool upscaleActive(FramePassContext& fc, const ViewResources& view);
// The view at the output's size (the post chain after the upscale writes view.color at it).
ViewResources upscaleOutputView(FramePassContext& fc, const ViewResources& view);
// The output-resolution image (RGBA16F, exposed linear; it is also the next frame's history) from 'src' (the view's
// exposed-linear image at the internal resolution) and the previous output.
TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src);
} // namespace unx::render::shading
