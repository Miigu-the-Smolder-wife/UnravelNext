#pragma once
// M track, shading (ARCHITECTURE 2.11; INTERFACES 5.2, 7.5): per shade class one kernel over that class's tiles
// (material resolve's lists, ExecuteIndirect), writing the view's final colour.
#include "unx/render/Frame.h"

#include <vector>

namespace unx::render::shading
{
// Specular directional albedo split by f0: the model's table (scene::model::specularAlbedoTable, the frame constant
// g_specularAlbedoLut since v1.25), 32 x 32 (A, B) on the model's E grid, A + B = E to float rounding.
const std::vector<float>& specularAlbedoTable();
// LTC inverse matrices of the model's specular lobe for area lights (AreaLight.hlsli): 64 x 64 float4, fitted by
// Tests/LtcFit.cpp (LtcTable.inl).
const std::vector<float>& ltcTable();

// The shading of 'view' in two parts around a banded pass group (INTERFACES v1.29): shadingPasses records the passes
// before the group (ShadeBegin) and returns the banded ones (edge detection, then the shading kernels over the band's
// tiles; neither needs a lag), shadingComposite records what reads the whole view after the group (overflow fallback
// tiles, statistics, edge composite). shade() is both around M's own group ("m.lit").
std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view);
void shadingComposite(FramePassContext& fc, ViewResources& view);
void shade(FramePassContext& fc, ViewResources& view);

// Tile counts of the main view's latest recorded frame (read back through a ring; valid once that frame completed,
// e.g. after the device went idle): shade classes of the resolve and edge pixels of the shading kernels.
struct Stats
{
    uint64_t frameIndex = UINT64_MAX;
    uint32_t classTiles[4] = {};
    uint32_t edgePixels = 0;
    uint32_t tiles = 0;
};
Stats latestStats(TrackState& state);
} // namespace unx::render::shading
