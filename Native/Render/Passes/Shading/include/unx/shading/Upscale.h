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
// The previous frame's upscaled colour (the history temporalUpscale reads this frame) in this frame's graph, for passes
// recorded before the upscale - screen-space traces of reflection and GI rays (ViewResources::prevSceneColor documents
// the texture). Imported once per frame; temporalUpscale then uses the same reference. Invalid when the view is not
// upscaled or the history holds nothing yet.
TextureRef upscalePreviousColor(FramePassContext& fc, const ViewResources& view);
// What the upscale leaves for the passes after it (the motion blur at the output resolution): each internal sample's
// output-UV offset to the previous frame (RG32F, not dilated; UpscaleMotion.hlsl) and the device depth of the surface
// that vector is of (the view's depth, or the tracked depth where a layer has the pixel).
struct UpscaleProducts
{
    TextureRef motion, depth;
};
TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src, UpscaleProducts* products = nullptr);
// output.screen_trace_source = 0: keeps 'lit' - the view's opaque image right after the shading group, before water,
// the coverage layers and glass - with the air taken off, as the scene colour upscalePreviousColor gives the next
// frame's screen traces (UpscaleSceneKeep.hlsl). Recorded before temporalUpscale in the frame.
void keepSceneColor(FramePassContext& fc, const ViewResources& view, TextureRef lit);
} // namespace unx::render::shading
