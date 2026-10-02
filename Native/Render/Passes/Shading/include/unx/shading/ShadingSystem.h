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
// shadingScatter (shading.subsurface_scatter; scenes with Subsurface materials) records the Subsurface class's scatter
// pass, which writes that class's pixels: after the group (it reads the class's diffuse light around each pixel) and
// before anything that reads the lit image (the kept scene colour, the water's refraction source). A caller that leaves
// it out gets it at the start of shadingComposite.
std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view);
void shadingScatter(FramePassContext& fc, ViewResources& view);
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
    uint32_t coverageErrors = 0;  // coverage composite error bits (0x400: a data-dependent loop reached its bound, 0x800: a heavy pixel open after the rounds)
    // The coverage composite's work of the frame: light pixels (1..16 records) and heavy ones, the records the light pixels'
    // walks visited (nearer first, until the pixel is complete or band A) and those of them with weight (shaded); the
    // compact form's entries (shading.coverage_compact: = the shaded ones; 0 in the composite's form).
    uint32_t coverageLightPixels = 0, coverageHeavyPixels = 0, coverageWalked = 0, coverageShaded = 0, coverageEntries = 0;
    // A10 glass over the translucent layer (TranslucentComposite.hlsl, class 1 pixels): panes composited, solid glass
    // drawn without refraction (until R's refraction rays), pixels whose sun visibility had no resident VSM page (lit).
    uint32_t glassPanePixels = 0, glassSolidPixels = 0, glassUnlitPixels = 0;
    // A5 depth of field (DepthOfField.cpp): pixels gathered (a blurred source reaches them), pixels whose circle of
    // confusion was clamped to the 512 px radius bound.
    uint32_t dofPixels = 0, dofClampedPixels = 0;
};
Stats latestStats(TrackState& state);
} // namespace unx::render::shading
