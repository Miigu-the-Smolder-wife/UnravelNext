#pragma once
// M's motion blur (MotionBlur.cpp; COVERAGE 14.12 (2), render A item A5): the shutter's time integral.
#include "unx/render/Frame.h"

namespace unx::render::shading
{
// The shutter is open for this view (the main view, shading.motion_blur_shutter > 0) and the frame has motion (the camera
// moved or GpuScene::hasMotion). Static frames skip it (their image is the integral).
bool motionBlurActive(FramePassContext& fc, const ViewResources& view);
// Screen velocity of the view (RG16F, pixels per frame interval; MotionVelocity.hlsl). Tests read it.
TextureRef motionVelocity(FramePassContext& fc, const ViewResources& view);
// The exposure integral of 'src' (exposed linear radiance) into 'dst' (same size, a float format).
void motionBlur(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst);
// shading.motion_blur_after_upscale: an upscaled view's blur runs after the temporal upscale, at the output resolution
// (motionBlurUpscaled), in place of motionBlur before it.
bool motionBlurAfterUpscale(FramePassContext& fc);
// The exposure integral of the upscaled image 'src' into 'dst' (both at the output resolution, a float format), centred
// on the frame's time. view: the internal view; motion: its samples' output-UV offsets to the previous frame (RG32F,
// shading::UpscaleProducts::motion); depth: the device depth of the surfaces those vectors are of.
void motionBlurUpscaled(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, TextureRef motion, TextureRef depth);
// ... with a given velocity (RG16F, pixels per frame) and the view's depth (tests; motionBlur uses motionVelocity).
void motionBlurWithVelocity(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, TextureRef velocity);
// The rotation stage alone for a given rotation Q (rows; view space, R_cur R_prev^T) - tests; false when Q does not engage it
// (its streak at the image centre <= 16 px, or its axis in the view).
bool motionRotationBlur(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst, const float3 (&q)[3]);

// Heat haze (FEATURES_GAME 0.A-8; E's distortionOffset / distortionDepth on the main view): Distortion.hlsl.
bool distortionActive(FramePassContext& fc, const ViewResources& view);
void distortion(FramePassContext& fc, const ViewResources& view, TextureRef src, TextureRef dst);
} // namespace unx::render::shading
