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
// output.lens_panini_d / lens_panini_s (the reference's r.LensDistortion.Panini.D / S): the lens projection the upscaled
// picture is in (Lens.hlsli) - with output.upscale_tsr on an upscaled main view; the history update applies it, so the
// passes after the upscale work on the lens picture and the ones that read the rendered view's data per pixel
// (the motion blur's vectors) go through lensToRendered. Inactive: the picture is the rendered view's.
struct LensProjection
{
    bool active = false;
    float tanX = 0, tanY = 0;  // of the rendered view's half field of view
    float d = 0, s = 0;
    float scale = 0;           // the centre's magnification (the rendered width fills the picture)
};
LensProjection upscaleLens(FramePassContext& fc, const ViewResources& view);
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
    BufferRef status;  // UpscaleMotion::status
};
TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src, UpscaleProducts* products = nullptr);
// The upscale's vectors (m.upscale.motion, UpscaleMotion.hlsl) for a pass recorded before temporalUpscale that
// reprojects the internal-resolution picture by each pixel's own motion (the diaphragm depth of field's prefilter):
// the pass is recorded at the first call of a frame, and temporalUpscale, the last, takes the same textures. They
// read the view's depth, its vis buffer and the layers over the opaque surface, all complete once the view is
// composed. Invalid references when the view is not upscaled.
struct UpscaleMotion
{
    TextureRef motion;         // RG32F per internal pixel: its unjittered UV now - its UV in the previous frame
    TextureRef depth;          // the device depth of the surface each vector is of (the view's, or the layers')
    TextureRef previousDepth;  // (output.upscale_tsr) the point's view depth in the previous frame, how much it moves
    TextureRef layers;         // (output.upscale_layer_motion) the pixel's layers
    BufferRef status;          // (with the coverage layer) raw, word 0: pixels whose coverage records were more than the
                               // kernel reads (UpscaleMotion.hlsl LAYER_FRAGMENTS); invalid: nothing to count
    bool layerMotion = false;
};
UpscaleMotion upscaleMotion(FramePassContext& fc, const ViewResources& view);
// output.screen_trace_source = 0: keeps 'lit' - the view's opaque image right after the shading group, before water,
// the coverage layers and glass - with the air taken off, as the scene colour upscalePreviousColor gives the next
// frame's screen traces (UpscaleSceneKeep.hlsl). Recorded before temporalUpscale in the frame.
void keepSceneColor(FramePassContext& fc, const ViewResources& view, TextureRef lit);
} // namespace unx::render::shading
