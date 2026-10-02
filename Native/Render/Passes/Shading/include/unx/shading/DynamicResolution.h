#pragma once
// M's dynamic resolution (DynamicResolution.cpp; output.dynamic_resolution_target_ms): the internal height of the
// temporally upscaled main view chosen each frame from the GPU time of the frames before, to hold a frame time budget.
#include "unx/render/Frame.h"
#include "unx/render/Tracks.h"

namespace unx::render::shading
{
// The main view's internal height of this frame: 'maxHeight' (the static settings' height: output.render_scale,
// output.render_height_max) when the controller is off or has no timing yet, else a height between its minimum and
// maxHeight in steps of output.dynamic_resolution_step_lines. Called once per frame, before the frame's constants
// (FrameRenderer::setupUpscale); frame.timing is the newest completed frame's GPU time.
uint32_t dynamicResolutionHeight(TrackState& state, const QualityConfig& quality, const FrameContext& frame, uint32_t outputHeight, uint32_t maxHeight);
// What the controller decided last (a host reads it: UnxFrameGetStatistics).
tracks::DynamicResolutionStatus dynamicResolutionStatus(TrackState& state);
} // namespace unx::render::shading
