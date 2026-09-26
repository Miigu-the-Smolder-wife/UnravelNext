#pragma once
// Automatic exposure of M (FEATURES_GAME 6.2, render A item A4): the histogram the shading kernels fill, its readback,
// and the metering and adaptation that give each frame's EV100 (Exposure.cpp).
#include "unx/render/FrameContext.h"
#include "unx/render/GraphTypes.h"

#include <cstdint>

namespace unx
{
class QualityConfig;
}
namespace unx::render
{
class Device;
class TrackState;
struct FramePassContext;
}

namespace unx::render::shading
{
// 64 bins of log2 luminance (nit) from kExposureLog2Min in steps of kExposureLog2Step (ShadingCommon.hlsli matches):
// 2^-8 to 2^24 nit, a moonless night to the sun's disk.
constexpr float kExposureLog2Min = -8.0f, kExposureLog2Step = 0.5f;

struct ExposureHistogram
{
    BufferRef buffer;         // raw, 64 x uint: centre-weighted pixel counts (x 64)
    float centreSigma = 0.5f;  // shading.exposure_centre_sigma
};

// This frame's histogram (imported; cleared on first use, after each frame's readback thereafter).
ExposureHistogram exposureHistogram(FramePassContext& fc);
// After the frame's shading: copies the histogram into the readback ring and clears it.
void exposureReadback(FramePassContext& fc, const ExposureHistogram& histogram);
// The frame's EV100 (FrameRenderer, before any frame constants): metered from the histogram of frame
// frameIndex - framesInFlight and adapted over frame.deltaTime; the host's value until the first histogram.
float autoExposureEv100(TrackState& state, Device& device, const QualityConfig& quality, const FrameContext& frame, uint32_t framesInFlight);
// Metering of one histogram (tests): the EV100 that maps the mean log2 luminance between the cuts onto the target grey.
float meterEv100(const uint32_t* bins, float targetGrey, float cutLow, float cutHigh, bool& valid);
} // namespace unx::render::shading
