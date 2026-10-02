#pragma once
// Volumetric clouds in the frame (B5; S_STATUS_KO.md 9): the noise textures, the main view's cloud layer (persistent
// quarter-resolution textures whose SRVs the atmosphere record names, so S's readers composite them), and the per-frame
// passes (record, sun map, march). Default: no clouds (coverage 0); play mode keeps them off while their cost structure
// is being reduced (coordination decision 2026-09-27), captures and look development turn them on. Owner: S.
#include "CloudModel.h"
#include "unx/render/Frame.h"

namespace unx::render::atmosphere
{
// FrameContext::clouds (v1.77) sets the layer each frame; gates and tests without a host can set it here instead (kept
// until a frame brings a layer with coverage).
void setCloudLayer(TrackState& state, const clouds::CloudLayer& layer);

// Before the atmosphere record is compared: the cloud layer's SRVs + 1 (0, 0 without clouds); allocates the persistent
// layer textures at the main view's quarter resolution when clouds are on.
void cloudsPrepare(FramePassContext& fc, uint32_t& recordSrv);
// After the atmosphere LUTs: this frame's cloud passes (nothing without clouds). tlutSrvSource: the transmittance LUT.
void cloudsRecord(FramePassContext& fc, TextureRef transmittanceLut);

// The frame's cloud lighting word for the kernels that light the cloud (CloudMarch.hlsl P[3].w, FogIntegrate.hlsl):
// atmosphere.clouds.sun_steps | filtered_steps << 8 | ground_light << 9.
uint32_t cloudSunWord(const QualityConfig& quality);

struct CloudStats
{
    uint32_t cappedPixels = 0, cappedSunPaths = 0;  // the last read frame's step bounds reached (CloudMarch.hlsl)
    uint64_t frames = 0;
};
CloudStats cloudStats(TrackState& state);  // blocking read of the last frame's counters (tests, gates)
} // namespace unx::render::atmosphere
